#include "ScreenRecorder.h"
#include "MacRecordingSource.h"
#include "platform/MediaPermissions_mac.h"
#include "utils/MediaComposition_mac.h"
#include <SDL3/SDL.h>
#import <AVFoundation/AVFoundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <AppKit/AppKit.h>
#include <atomic>

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
API_AVAILABLE(macos(15.0))
@interface ShotletRecordingObserver : NSObject <SCRecordingOutputDelegate, SCStreamDelegate>
@property(nonatomic, strong) dispatch_semaphore_t startedSignal;
@property(nonatomic, strong) dispatch_semaphore_t finishedSignal;
@property(atomic, copy) NSString* failure;
@property(atomic) BOOL started;
@property(atomic) BOOL finished;
@property(atomic) BOOL userStopped;
@end
@implementation ShotletRecordingObserver
- (instancetype)init {
    if ((self = [super init])) {
        _startedSignal = dispatch_semaphore_create(0);
        _finishedSignal = dispatch_semaphore_create(0);
    }
    return self;
}
- (void)recordingOutputDidStartRecording:(SCRecordingOutput*)output {
    self.started = YES;
    dispatch_semaphore_signal(self.startedSignal);
}
- (void)recordingOutputDidFinishRecording:(SCRecordingOutput*)output {
    self.finished = YES;
    dispatch_semaphore_signal(self.finishedSignal);
}
- (void)recordingOutput:(SCRecordingOutput*)output didFailWithError:(NSError*)error {
    self.failure = error.localizedDescription;
    dispatch_semaphore_signal(self.startedSignal);
    dispatch_semaphore_signal(self.finishedSignal);
}
- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    if ([error.domain isEqualToString:SCStreamErrorDomain] && error.code == SCStreamErrorUserStopped)
        self.userStopped = YES;
    else self.failure = error.localizedDescription;
    dispatch_semaphore_signal(self.startedSignal);
}
@end
#endif

namespace shotlet {
namespace {

class MacScreenRecorder final : public IScreenRecorder {
public:
    ~MacScreenRecorder() override { std::string ignored; Stop(ignored); }
    bool Start(const RecordingOptions& options, const std::filesystem::path& path, std::string& error) override;
    bool Stop(std::string& error) override;
    bool SetPaused(bool paused, std::string& error) override;
    bool HasStopped() const override {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
        if (@available(macOS 15.0, *)) return (m_output && m_observer.finished) || m_observer.userStopped;
#endif
        return false;
    }
    std::string GetError() const override {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
        if (@available(macOS 15.0, *)) {
            NSString* failure = m_observer.failure;
            return failure ? failure.UTF8String : "";
        }
#endif
        return {};
    }
private:
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
    bool AddOutput(std::string& error) API_AVAILABLE(macos(15.0));
    bool WaitForOutput(std::string& error) API_AVAILABLE(macos(15.0));
    bool FinishOutput(std::string& error) API_AVAILABLE(macos(15.0));
    SCStream* m_stream API_AVAILABLE(macos(15.0)) = nil;
    SCRecordingOutput* m_output API_AVAILABLE(macos(15.0)) = nil;
    ShotletRecordingObserver* m_observer API_AVAILABLE(macos(15.0)) = nil;
#endif
    std::filesystem::path m_path;
    // 每段各自持有临时目录，任何启动、恢复或拼接失败都会自动清理。
    std::vector<std::unique_ptr<AtomicFile>> m_segments;
};

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
bool MacScreenRecorder::AddOutput(std::string& error) {
    auto segment = std::make_unique<AtomicFile>();
    if (!segment->Prepare(m_path, error)) return false;
    m_observer.started = NO; m_observer.finished = NO;
    m_observer.startedSignal = dispatch_semaphore_create(0);
    m_observer.finishedSignal = dispatch_semaphore_create(0);
    auto* configuration = [[SCRecordingOutputConfiguration alloc] init];
    configuration.outputURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:segment->Path().c_str()]];
    configuration.outputFileType = AVFileTypeMPEG4;
    configuration.videoCodecType = AVVideoCodecTypeH264;
    m_output = [[SCRecordingOutput alloc] initWithConfiguration:configuration delegate:m_observer];
    NSError* failure = nil;
    if (![m_stream addRecordingOutput:m_output error:&failure]) {
        error = failure.localizedDescription.UTF8String ?: "无法创建 MP4 录制输出";
        m_output = nil; return false;
    }
    m_segments.push_back(std::move(segment));
    return true;
}

bool MacScreenRecorder::WaitForOutput(std::string& error) {
    if (MacRecordingSource::Wait(m_observer.startedSignal) && m_observer.started) return true;
    error = GetError();
    if (error.empty()) error = "启动 MP4 编码器超时";
    m_observer.failure = [NSString stringWithUTF8String:error.c_str()];
    return false;
}

bool MacScreenRecorder::FinishOutput(std::string& error) {
    if (!m_output) {
        if (error.empty()) error = GetError();
        return error.empty();
    }
    // 停止采集或移除输出都不代表文件已经封装完，必须等待完成回调。
    if (m_observer.started && !m_observer.finished && !MacRecordingSource::Wait(m_observer.finishedSignal))
        error = "保存 MP4 文件超时";
    if (error.empty()) error = GetError();
    m_output = nil;
    return error.empty();
}
#endif

bool MacScreenRecorder::Start(const RecordingOptions& options, const std::filesystem::path& path, std::string& error) {
    @autoreleasepool {
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
    if (@available(macOS 15.0, *)) {
        SCContentFilter* filter = nil;
        SCStreamConfiguration* configuration = nil;
        if (!MacRecordingSource::Configure(options, filter, configuration, error)) return false;
        m_path = path; m_segments.clear();
        m_observer = [[ShotletRecordingObserver alloc] init];
        m_stream = [[SCStream alloc] initWithFilter:filter configuration:configuration delegate:m_observer];
        if (!AddOutput(error)) { m_stream = nil; return false; }
        __block NSError* startError = nil;
        auto started = dispatch_semaphore_create(0);
        auto abandoned = std::make_shared<std::atomic_bool>(false);
        SCStream* stream = m_stream;
        [stream startCaptureWithCompletionHandler:^(NSError* failure) {
            startError = failure;
            // 超时后回调仍可能到达；由回调持有原生对象并收掉迟到的流。
            if (abandoned->load()) [stream stopCaptureWithCompletionHandler:nil];
            dispatch_semaphore_signal(started);
        }];
        if (!MacRecordingSource::Wait(started)) {
            abandoned->store(true); error = "启动屏幕录制超时";
            std::string ignored; Stop(ignored); return false;
        }
        if (startError || !WaitForOutput(error)) {
            if (startError) error = startError.localizedDescription.UTF8String;
            std::string ignored; Stop(ignored); return false;
        }
        return true;
    }
#endif
    error = "屏幕录制需要 macOS 15 或更新版本";
    return false;
    }
}

bool MacScreenRecorder::SetPaused(bool paused, std::string& error) {
    @autoreleasepool {
    error.clear();
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
    if (@available(macOS 15.0, *)) {
        if (!m_stream || HasStopped()) { error = "屏幕录制已停止"; return false; }
        if (paused == (m_output == nil)) return true;
        if (!paused) return AddOutput(error) && WaitForOutput(error);
        NSError* failure = nil;
        // ScreenCaptureKit 没有录制暂停接口；暂停封装当前片段，继续时在同一采集流上开启新片段。
        if (![m_stream removeRecordingOutput:m_output error:&failure]) {
            error = failure.localizedDescription.UTF8String ?: "无法暂停屏幕录制"; return false;
        }
        const bool finished = FinishOutput(error);
        if (!finished) m_observer.failure = [NSString stringWithUTF8String:error.c_str()];
        return finished;
    }
#endif
    error = "当前系统不支持暂停录制"; return false;
    }
}

bool MacScreenRecorder::Stop(std::string& error) {
    @autoreleasepool {
    error.clear();
#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 150000
    if (@available(macOS 15.0, *)) {
        if (!m_stream) return true;
        __block NSError* stopError = nil;
        auto stopped = dispatch_semaphore_create(0);
        if (!m_observer.userStopped) {
            [m_stream stopCaptureWithCompletionHandler:^(NSError* failure) {
                stopError = failure; dispatch_semaphore_signal(stopped);
            }];
            if (!MacRecordingSource::Wait(stopped)) error = "停止屏幕采集超时";
            else if (stopError && !HasStopped()) error = stopError.localizedDescription.UTF8String;
        }
        FinishOutput(error);
        m_stream = nil; m_output = nil;
        if (error.empty() && !m_segments.empty()) {
            if (m_segments.size() == 1) m_segments.front()->Commit(error);
            else {
                std::vector<std::filesystem::path> paths;
                for (const auto& segment : m_segments) paths.push_back(segment->Path());
                MediaComposition::ConcatenateMp4(paths, m_path, error);
            }
        }
        m_segments.clear();
        return error.empty();
    }
#endif
    return true;
    }
}
}

std::unique_ptr<IScreenRecorder> CreateMacGifRecorder();
std::unique_ptr<IScreenRecorder> CreateScreenRecorder(RecordingFormat format) {
    return format == RecordingFormat::Gif ? CreateMacGifRecorder() : std::make_unique<MacScreenRecorder>();
}
void ExcludeRecordingControlWindow(SDL_Window*) {}
uint32_t RecordingControlWindowId(SDL_Window* window) {
    NSWindow* native = (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    return uint32_t(native.windowNumber);
}
bool IsScreenRecordingSupported() {
    if (@available(macOS 15.0, *)) return true;
    return false;
}
bool PrepareRecordingPermissions(std::string& error, bool microphone) {
    error.clear();
    if (@available(macOS 15.0, *)) {
        if (!CGPreflightScreenCaptureAccess() && !CGRequestScreenCaptureAccess()) {
            error = "请在系统设置的隐私与安全性中允许屏幕与系统音频录制"; return false;
        }
        if (!microphone) return true;
        if (!RequestMediaDevicePermission(AVMediaTypeAudio, @"NSMicrophoneUsageDescription", "麦克风", error)) return false;
        if (![AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio]) {
            error = "未找到可用麦克风"; return false;
        }
        return true;
    }
    error = "屏幕录制需要 macOS 15 或更新版本";
    return false;
}
}
