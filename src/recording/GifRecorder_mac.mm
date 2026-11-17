#include "ScreenRecorder.h"
#include "MacRecordingSource.h"
#include "GifWriter.h"
#include "utils/ScopeExit.h"
#include "utils/PausableClock.h"
#include <atomic>
#include <chrono>
#include <mutex>

namespace shotlet {
struct GifCaptureState {
    GifWriter writer;
    std::mutex frameMutex, errorMutex;
    std::string error;
    std::atomic_bool stopping{false}, stopped{false};
    int width = 0, height = 0;
    bool hasFrame = false;
    PausableClock clock;
    CVPixelBufferRef pausedFrame = nullptr;
    dispatch_semaphore_t firstFrameSignal = dispatch_semaphore_create(0);
    ~GifCaptureState() { if (pausedFrame) CVPixelBufferRelease(pausedFrame); }
    void Fail(std::string failure) { std::lock_guard lock(errorMutex); if (error.empty()) error = std::move(failure); }
    // 调用方持有 frameMutex；暂停时只保留最后一张原生缓冲，不复制整帧或积压采集队列。
    void CachePausedFrame(CVPixelBufferRef pixels) {
        if (pausedFrame) CVPixelBufferRelease(pausedFrame);
        pausedFrame = CVPixelBufferRetain(pixels);
    }
    bool AppendFrame(CVPixelBufferRef pixels) {
        if (CVPixelBufferLockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
            Fail("无法读取 GIF 画面"); return false;
        }
        const ScopeExit unlock([&] { CVPixelBufferUnlockBaseAddress(pixels, kCVPixelBufferLock_ReadOnly); });
        if (CVPixelBufferGetWidth(pixels) != width || CVPixelBufferGetHeight(pixels) != height ||
            CVPixelBufferGetPixelFormatType(pixels) != kCVPixelFormatType_32BGRA) {
            Fail("GIF 画面尺寸或像素格式已改变"); return false;
        }
        const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(clock.Elapsed()).count();
        std::string failure;
        try {
            if (writer.AddBgraFrame(static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixels)),
                int(CVPixelBufferGetBytesPerRow(pixels)), false, elapsedMs, failure)) return true;
        } catch (const std::exception& exception) { failure = exception.what(); }
        Fail(std::move(failure));
        return false;
    }
};
}

@interface ShotletGifOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@property(nonatomic) std::shared_ptr<shotlet::GifCaptureState> state;
@end
@implementation ShotletGifOutput
- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type {
    @autoreleasepool {
        auto state = self.state;
        if (type != SCStreamOutputTypeScreen || state->stopping.load() || !CMSampleBufferIsValid(sample)) return;
        CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
        if (attachments && CFArrayGetCount(attachments)) {
            NSDictionary* values = (__bridge NSDictionary*)CFArrayGetValueAtIndex(attachments, 0);
            if ([values[SCStreamFrameInfoStatus] integerValue] != SCFrameStatusComplete) return;
        }
        auto* pixels = CMSampleBufferGetImageBuffer(sample);
        if (!pixels) return;
        std::lock_guard lock(state->frameMutex);
        if (state->stopping.load()) return;
        if (state->clock.IsPaused()) { state->CachePausedFrame(pixels); return; }
        const bool first = !state->hasFrame;
        if (first) { state->hasFrame = true; state->clock.Reset(); }
        // GIF 帧间隔与结束时长共用可暂停时钟，恢复后不会保留暂停期间的长停帧。
        state->AppendFrame(pixels);
        if (first) dispatch_semaphore_signal(state->firstFrameSignal);
    }
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    auto state = self.state;
    if (error.code != SCStreamErrorUserStopped) state->Fail(error.localizedDescription.UTF8String);
    state->stopped.store(true); dispatch_semaphore_signal(state->firstFrameSignal);
}
@end

namespace shotlet {
namespace {
class MacGifRecorder final : public IScreenRecorder {
public:
    ~MacGifRecorder() override { std::string ignored; Stop(ignored); }
    bool Start(const RecordingOptions& options, const std::filesystem::path& path, std::string& error) override {
        @autoreleasepool {
        if (@available(macOS 15.0, *)) {
            m_state = std::make_shared<GifCaptureState>();
            m_state->width = options.widthPixels; m_state->height = options.heightPixels;
            if (!m_state->writer.Open(path, options.widthPixels, options.heightPixels, error)) return false;
            SCContentFilter* filter = nil; SCStreamConfiguration* configuration = nil;
            if (!MacRecordingSource::Configure(options, filter, configuration, error)) return false;
            configuration.pixelFormat = kCVPixelFormatType_32BGRA;
            m_output = [[ShotletGifOutput alloc] init]; m_output.state = m_state;
            m_stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:m_output];
            auto queue = dispatch_queue_create("shotlet.recording.gif", DISPATCH_QUEUE_SERIAL);
            NSError* failure = nil;
            if (![m_stream addStreamOutput:m_output type:SCStreamOutputTypeScreen sampleHandlerQueue:queue error:&failure]) {
                error = failure.localizedDescription.UTF8String; m_stream = nil; return false;
            }
            __block NSError* startError = nil;
            auto ready = dispatch_semaphore_create(0);
            auto abandoned = std::make_shared<std::atomic_bool>(false);
            SCStream* stream = m_stream;
            [stream startCaptureWithCompletionHandler:^(NSError* failure) {
                startError = failure;
                if (abandoned->load()) [stream stopCaptureWithCompletionHandler:nil];
                dispatch_semaphore_signal(ready);
            }];
            if (!MacRecordingSource::Wait(ready)) { abandoned->store(true); error = "GIF 录制启动超时"; }
            else if (startError) error = startError.localizedDescription.UTF8String;
            else if (!MacRecordingSource::Wait(m_state->firstFrameSignal)) error = "未收到 GIF 首帧";
            if (error.empty()) error = GetError();
            if (!error.empty()) { std::string ignored; Stop(ignored); return false; }
            return true;
        }
        error = "屏幕录制需要 macOS 15 或更新版本"; return false;
        }
    }
    bool Stop(std::string& error) override {
        @autoreleasepool {
        error.clear();
        if (!m_stream) return true;
        m_state->stopping.store(true);
        {
            std::lock_guard lock(m_state->frameMutex);
            m_state->clock.SetPaused(true);
        }
        if (!m_state->stopped.load()) {
            auto ready = dispatch_semaphore_create(0);
            [m_stream stopCaptureWithCompletionHandler:^(NSError*) { dispatch_semaphore_signal(ready); }];
            if (!MacRecordingSource::Wait(ready)) error = "停止 GIF 录制超时";
        }
        std::lock_guard lock(m_state->frameMutex);
        if (error.empty()) error = GetError();
        if (error.empty()) {
            const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(m_state->clock.Elapsed()).count();
            m_state->writer.Finish(durationMs, error);
        }
        m_stream = nil; m_output = nil;
        return error.empty();
        }
    }
    bool SetPaused(bool paused, std::string& error) override {
        error.clear();
        if (!m_state || m_state->stopping.load() || m_state->stopped.load()) { error = "GIF 录制已停止"; return false; }
        std::lock_guard lock(m_state->frameMutex);
        if (paused == m_state->clock.IsPaused()) return true;
        m_state->clock.SetPaused(paused);
        // 静止画面不会在恢复后自动产生 Complete 帧，立即写入暂停期间缓存的最新画面。
        if (!paused && m_state->pausedFrame) {
            const bool appended = m_state->AppendFrame(m_state->pausedFrame);
            CVPixelBufferRelease(m_state->pausedFrame); m_state->pausedFrame = nullptr;
            if (!appended) { error = GetError(); return false; }
        }
        return true;
    }
    std::string GetError() const override {
        if (!m_state) return {};
        std::lock_guard lock(m_state->errorMutex); return m_state->error;
    }
    bool HasStopped() const override { return m_state && m_state->stopped.load(); }
private:
    std::shared_ptr<GifCaptureState> m_state;
    SCStream* m_stream = nil;
    ShotletGifOutput* m_output = nil;
};
}
std::unique_ptr<IScreenRecorder> CreateMacGifRecorder() { return std::make_unique<MacGifRecorder>(); }
}
