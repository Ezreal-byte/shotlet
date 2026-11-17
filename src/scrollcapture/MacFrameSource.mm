// MacFrameSource：ScreenCaptureKit 持续流采集实现（设计 10.1，spike A）。
// 生命周期/semaphore 同步/void* 桥接范式来自 capture/backends/CaptureMacOS_SC.mm。

#include "scrollcapture/MacFrameSource.h"
#include "utils/Logger.h"

#if defined(SHOTLET_PLATFORM_MACOS)

#import <CoreGraphics/CoreGraphics.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <Accelerate/Accelerate.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>
#include <unistd.h>
#include <mach/mach_time.h>

#if __MAC_OS_X_VERSION_MAX_ALLOWED >= 120300

namespace shotlet {

namespace {

using Clock = std::chrono::steady_clock;

}  // namespace

}  // namespace shotlet

// SCStream 帧回调 + 流错误代理，转发到 C++ 侧（owner 生命周期由 MacFrameSource 保证）。
// ObjC 声明必须在全局作用域（不能放 C++ namespace 内）
@interface MacFrameSourceStreamOutput : NSObject <SCStreamOutput, SCStreamDelegate>
@property (nonatomic, assign) shotlet::MacFrameSource* owner;
@end

@implementation MacFrameSourceStreamOutput

- (void)stream:(SCStream*)stream didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
        ofType:(SCStreamOutputType)type {
    @synchronized(self) {
    if (type != SCStreamOutputTypeScreen || !self.owner) {
        return;
    }

    // Idle 表示画面未变化，仍需通知等待方，否则滚到边界会被误判为采集失败。
    uint64_t displayTime = 0;
    CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, false);
    if (attachments && CFArrayGetCount(attachments) > 0) {
        CFDictionaryRef props =
            static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(attachments, 0));
        CFNumberRef timeRef = static_cast<CFNumberRef>(CFDictionaryGetValue(
            props, (__bridge const void*)SCStreamFrameInfoDisplayTime));
        int64_t ticks = 0;
        if (!timeRef || !CFNumberGetValue(timeRef, kCFNumberSInt64Type, &ticks) || ticks <= 0) {
            return;  // 不用回调时间冒充采集时间，否则排队旧帧会被误认成本步响应。
        }
        displayTime = static_cast<uint64_t>(ticks);
        CFNumberRef statusRef = static_cast<CFNumberRef>(CFDictionaryGetValue(
            props, (__bridge const void*)SCStreamFrameInfoStatus));
        int status = 0;
        if (statusRef && CFNumberGetValue(statusRef, kCFNumberIntType, &status) &&
            status != SCFrameStatusComplete) {
            if (status == SCFrameStatusIdle) {
                self.owner->HandleIdleFrame(displayTime);
            }
            return;
        }
    }

    if (displayTime == 0) return;

    CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
    if (!imageBuffer) {
        return;
    }

    CVPixelBufferRef pixelBuffer = static_cast<CVPixelBufferRef>(imageBuffer);
    if (CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
        // TODO(spike): LockBaseAddress 不可靠时退路 CIImage→CGImage 慢路径
        shotlet::Logger::Warning("[MacFrameSource] CVPixelBufferLockBaseAddress failed, drop frame");
        return;
    }

    const uint8_t* base =
        static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixelBuffer));
    const int width = static_cast<int>(CVPixelBufferGetWidth(pixelBuffer));
    const int height = static_cast<int>(CVPixelBufferGetHeight(pixelBuffer));
    const int pitch = static_cast<int>(CVPixelBufferGetBytesPerRow(pixelBuffer));
    const OSType format = CVPixelBufferGetPixelFormatType(pixelBuffer);

    if (base && width > 0 && height > 0) {
        const bool isBGRA = (format == kCVPixelFormatType_32BGRA);
        const bool isRGBA = (format == kCVPixelFormatType_32RGBA);
        if (isBGRA || isRGBA) {
            self.owner->HandleFrame(base, width, height, pitch, isBGRA, displayTime);
        } else {
            shotlet::Logger::Warning("[MacFrameSource] unsupported pixel format 0x{:x}, drop frame",
                            static_cast<uint32_t>(format));
        }
    }

    CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
    }
}

- (void)stream:(SCStream*)stream didStopWithError:(NSError*)error {
    @synchronized(self) {
    if (!self.owner) {
        return;
    }
    const char* message =
        error ? [error.localizedDescription UTF8String] : "unknown stream error";
    self.owner->HandleStreamError(message);
    }
}

@end

namespace shotlet {

MacFrameSource::~MacFrameSource() {
    Stop();
}

bool MacFrameSource::Start(uint32_t cgDisplayId, const GlobalRect& rect, bool excludeOwnWindows, const std::function<bool()>& cancelled) {
    @autoreleasepool {
    if (m_stream) {
        Stop();
    }
    if (cancelled && cancelled()) return false;
    if (rect.w <= 0 || rect.h <= 0) {
        m_lastError = "invalid capture rect";
        return false;
    }
    if (@available(macOS 12.3, *)) {
        // ScreenCaptureKit 可用，继续
    } else {
        m_lastError = "ScreenCaptureKit requires macOS 12.3+";
        return false;
    }

    const auto wait = [&](dispatch_semaphore_t semaphore) {
        const auto deadline = Clock::now() + std::chrono::seconds(10);
        while (Clock::now() < deadline) {
            if (cancelled && cancelled()) return false;
            if (dispatch_semaphore_wait(semaphore, dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC)) == 0) return true;
        }
        return false;
    };
    // 会话级获取 SCShareableContent（单会话生命周期，不做进程级缓存）
    __block SCShareableContent* content = nil;
    __block NSError* contentError = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [SCShareableContent getShareableContentWithCompletionHandler:^(
                               SCShareableContent* shareable, NSError* error) {
        content = shareable;
        contentError = error;
        dispatch_semaphore_signal(sem);
    }];
    // 有界等待（CaptureMacOS_SC 同款 abandoned 处理：10s 覆盖首授权确认）
    if (!wait(sem)) {
        m_lastError = "getShareableContent timed out";
        return false;
    }
    if (!content) {
        m_lastError = contentError ? [contentError.localizedDescription UTF8String]
                                   : "failed to get shareable content";
        return false;
    }

    // 匹配目标显示器
    SCDisplay* targetDisplay = nil;
    for (SCDisplay* d in content.displays) {
        if (d.displayID == cgDisplayId) {
            targetDisplay = d;
            break;
        }
    }
    if (!targetDisplay) {
        m_lastError = "display not found in shareable content";
        return false;
    }

    // 排除自身窗口（设计 10.1：抓 overlay 下方的真实内容；pid 范式同 SmartRegionDetector）
    NSMutableArray<SCWindow*>* excluded = [NSMutableArray array];
    if (excludeOwnWindows) {
        const pid_t selfPid = getpid();
        for (SCWindow* w in content.windows) {
            if (w.owningApplication && w.owningApplication.processID == selfPid) {
                [excluded addObject:w];
            }
        }
    }

    // 排除进程而非启动瞬间的窗口快照，预览/面板后续创建也不会进入采集帧。
    NSMutableArray<SCRunningApplication*>* excludedApps = [NSMutableArray array];
    if (excludeOwnWindows) {
        for (SCRunningApplication* app in content.applications) {
            if (app.processID == getpid()) [excludedApps addObject:app];
        }
    }
    SCContentFilter* filter = [[SCContentFilter alloc] initWithDisplay:targetDisplay
        excludingApplications:excludedApps exceptingWindows:@[]];

    // 选区点 → 像素换算（CGDisplayMode 像素尺寸 / CGDisplayBounds 点尺寸）
    const CGRect displayBounds = CGDisplayBounds(cgDisplayId);
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(cgDisplayId);
    double scaleFactor = 1.0;
    if (mode) {
        scaleFactor = CGDisplayModeGetPixelWidth(mode) / displayBounds.size.width;
        CGDisplayModeRelease(mode);
    }

    // sourceRect：全局点 → 显示器局部点（TODO(spike): sourceRect 单位/原点语义实机复核；
    // 不通时退路：全显示流 + HandleFrame 内裁剪，接口不变）
    const CGRect localRect = CGRectMake(
        static_cast<CGFloat>(rect.x) - displayBounds.origin.x,
        static_cast<CGFloat>(rect.y) - displayBounds.origin.y,
        static_cast<CGFloat>(rect.w), static_cast<CGFloat>(rect.h));

    SCStreamConfiguration* config = [[SCStreamConfiguration alloc] init];
    config.sourceRect = localRect;
    // 强制 BGRA（默认 420v YUV 需软件色彩转换，拼接管线要 RGBA8）
    config.pixelFormat = kCVPixelFormatType_32BGRA;
    // 输出 = 选区像素尺寸（无缩放，避免亚像素误差，设计 11.5）
    config.width = static_cast<int>(std::llround(rect.w * scaleFactor));
    config.height = static_cast<int>(std::llround(rect.h * scaleFactor));
    config.minimumFrameInterval = CMTimeMake(1, 60);
    config.queueDepth = 3;
    config.showsCursor = NO;  // 光标不进帧（设计 10.1）

    MacFrameSourceStreamOutput* output = [[MacFrameSourceStreamOutput alloc] init];
    output.owner = this;
    dispatch_queue_t queue =
        dispatch_queue_create("shotlet.scrollcapture.frames", DISPATCH_QUEUE_SERIAL);

    SCStream* stream = [[SCStream alloc] initWithFilter:filter configuration:config delegate:output];
    NSError* addError = nil;
    [stream addStreamOutput:output type:SCStreamOutputTypeScreen sampleHandlerQueue:queue
                      error:&addError];
    if (addError) {
        m_lastError = [addError.localizedDescription UTF8String];
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_latestFrameId = 0;
        m_latestDisplayTime = 0;
        m_latestIdle = false;
        m_latestPixels.reset();
        m_frameWidth = 0;
        m_frameHeight = 0;
        m_streamFailed = false;
        m_lastError.clear();
    }


    m_stream = (__bridge_retained void*)stream;
    m_output = (__bridge_retained void*)output;
    m_queue = (__bridge_retained void*)queue;
    auto abandoned = std::make_shared<std::atomic<bool>>(false);
    __block NSError* startError = nil;
    dispatch_semaphore_t startSem = dispatch_semaphore_create(0);
    [stream startCaptureWithCompletionHandler:^(NSError* error) {
        startError = error;
        if (abandoned->load()) [stream stopCaptureWithCompletionHandler:^(NSError*) {}];
        dispatch_semaphore_signal(startSem);
    }];
    if (!wait(startSem)) {
        abandoned->store(true);
        Stop();
        m_lastError = "startCapture timed out";
        return false;
    }
    if (startError) {
        Stop();
        m_lastError = [startError.localizedDescription UTF8String];
        return false;
    }


    m_displayId = cgDisplayId;
    m_rect = rect;

    Logger::Info("[MacFrameSource] started display={} rect=({},{},{},{})pt scale={:.1f} "
                 "output={}x{}px excludedWindows={}",
                 cgDisplayId, rect.x, rect.y, rect.w, rect.h, scaleFactor,
                 static_cast<int>(config.width), static_cast<int>(config.height),
                 static_cast<int>(excluded.count));
    return true;
    }
}

void MacFrameSource::RequestStop() {
    std::lock_guard lock(m_frameMutex);
    m_streamFailed = true;
    m_lastError = "采集已取消";
    m_frameCv.notify_all();
}

void MacFrameSource::Stop() {
    @autoreleasepool {
    SCStream* stream = (__bridge_transfer SCStream*)m_stream;  // 所有权还给 ARC
    MacFrameSourceStreamOutput* output =
        (__bridge_transfer MacFrameSourceStreamOutput*)m_output;
    dispatch_queue_t queue = (__bridge_transfer dispatch_queue_t)m_queue;
    m_stream = nullptr;
    m_output = nullptr;
    m_queue = nullptr;
    if (!stream) {
        return;
    }

    // 与任何正在执行的帧/错误回调互斥，释放 C++ owner 前彻底摘除。
    @synchronized(output) { output.owner = nullptr; }
    __block NSError* stopError = nil;
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    [stream stopCaptureWithCompletionHandler:^(NSError* error) {
        stopError = error;
        dispatch_semaphore_signal(sem);
    }];
    dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC));
    if (stopError) {
        Logger::Warning("[MacFrameSource] stopCapture error: {}",
                        [stopError.localizedDescription UTF8String]);
    }
    [stream removeStreamOutput:output type:SCStreamOutputTypeScreen error:nil];

    // 清空串行队列中可能残留的帧回调，再摘除 owner（防 use-after-free）
    if (queue) {
        dispatch_sync(queue, ^{});
    }
    output.owner = nullptr;  // ARC 在作用域结束时释放 stream/output/queue

    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_streamFailed = false;
    }
    m_frameCv.notify_all();
    Logger::Info("[MacFrameSource] stopped");
    }
}

bool MacFrameSource::IsRunning() const {
    return m_stream != nullptr;
}

uint64_t MacFrameSource::CurrentFrameId() const {
    std::lock_guard<std::mutex> lock(m_frameMutex);
    return m_latestFrameId;
}

CaptureBaseline MacFrameSource::TakeBaseline() const {
    std::lock_guard<std::mutex> lock(m_frameMutex);
    CaptureBaseline baseline;
    baseline.frame.sharedPixels = m_latestPixels;
    baseline.frame.width = m_frameWidth;
    baseline.frame.height = m_frameHeight;
    baseline.frame.frameId = m_latestFrameId;
    baseline.frame.displayTime = m_latestDisplayTime;
    baseline.notBefore = mach_absolute_time();
    return baseline;
}

void MacFrameSource::HandleFrame(const uint8_t* data, int width, int height, int pitch,
                                 bool isBGRA, uint64_t displayTime) {
    const auto start = Clock::now();
    if (displayTime == 0) displayTime = mach_absolute_time();
    const size_t rowBytes = static_cast<size_t>(width) * 4;
    if (!data || width <= 0 || height <= 0 || pitch < int(rowBytes)) return;
    // 转换在发布锁外完成，不阻塞拼接线程获取最新帧。
    auto pixels = std::make_shared<std::vector<uint8_t>>(rowBytes * static_cast<size_t>(height));
    if (isBGRA) {
        vImage_Buffer source{const_cast<uint8_t*>(data), static_cast<vImagePixelCount>(height),
                             static_cast<vImagePixelCount>(width), static_cast<size_t>(pitch)};
        vImage_Buffer dest{pixels->data(), static_cast<vImagePixelCount>(height),
                           static_cast<vImagePixelCount>(width), rowBytes};
        const uint8_t channels[] = {2, 1, 0, 3};
        if (vImagePermuteChannels_ARGB8888(&source, &dest, channels, kvImageNoFlags) != kvImageNoError) {
            Logger::Warning("[MacFrameSource] channel conversion failed, drop frame");
            return;
        }
    } else for (int y = 0; y < height; ++y) {
        const uint8_t* src = data + static_cast<size_t>(y) * static_cast<size_t>(pitch);
        uint8_t* dst = pixels->data() + rowBytes * static_cast<size_t>(y);
        std::memcpy(dst, src, rowBytes);  // 已是 RGBA
    }
    std::shared_ptr<const std::vector<uint8_t>> previous;
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        if (m_streamFailed || displayTime < m_latestDisplayTime) return;
        m_latestDisplayTime = displayTime;
        previous = std::exchange(m_latestPixels, std::move(pixels));
        m_frameWidth = width;
        m_frameHeight = height;
        ++m_latestFrameId;
        m_latestIdle = false;
    }
    m_frameCv.notify_all();
    Logger::Debug("[ScrollCapture] conversion={}x{} elapsedMs={:.2f}", width, height,
        std::chrono::duration<double, std::milli>(Clock::now() - start).count());
}

void MacFrameSource::HandleIdleFrame(uint64_t displayTime) {
    if (displayTime == 0) displayTime = mach_absolute_time();
    std::lock_guard<std::mutex> lock(m_frameMutex);
    if (!m_latestPixels || m_streamFailed || displayTime < m_latestDisplayTime) {
        return;  // 首个完整帧之前没有可复用像素。
    }
    ++m_latestFrameId;
    m_latestDisplayTime = displayTime;
    m_latestIdle = true;
    m_frameCv.notify_all();
}

void MacFrameSource::HandleStreamError(const char* message) {
    {
        std::lock_guard<std::mutex> lock(m_frameMutex);
        m_streamFailed = true;
        if (message) {
            m_lastError = message;
        }
    }
    m_frameCv.notify_all();
    Logger::Error("[MacFrameSource] stream stopped: {}", message ? message : "unknown");
}

bool MacFrameSource::WaitForFrameReference(uint64_t afterFrameId, CapturedFrame& out, double timeoutMs) {
    const auto deadline = Clock::now() + std::chrono::duration<double, std::milli>(timeoutMs);
    std::unique_lock<std::mutex> lock(m_frameMutex);
    while (m_latestFrameId <= afterFrameId) {
        if (m_streamFailed) {
            return false;
        }
        if (m_frameCv.wait_until(lock, deadline) == std::cv_status::timeout) {
            if (m_latestFrameId > afterFrameId) {
                break;  // 超时竞态下已有新帧则取之
            }
            return false;
        }
    }
    if (m_streamFailed) {
        return false;
    }
    out.sharedPixels = m_latestPixels;
    out.width = m_frameWidth;
    out.height = m_frameHeight;
    out.frameId = m_latestFrameId;
    out.displayTime = m_latestDisplayTime;
    out.stable = m_latestIdle;
    return true;
}

bool MacFrameSource::WaitForNextFrame(uint64_t afterFrameId, CapturedFrame& out, double timeoutMs) {
    if (!WaitForFrameReference(afterFrameId, out, timeoutMs)) return false;
    out.pixels = *out.sharedPixels; // 兼容需要独占像素的旧调用，复制在锁外。
    out.sharedPixels.reset();
    return true;
}

bool MacFrameSource::WaitForStableFrame(const CaptureBaseline& baseline, CapturedFrame& out) {
    const auto start = Clock::now();
    const auto deadline = start + std::chrono::duration<double, std::milli>(m_config.settleTimeoutMs);
    const auto quietPeriod = std::chrono::duration<double, std::milli>(m_config.quietPeriodMs);
    uint64_t lastSeen = baseline.frame.frameId;
    CapturedFrame latest;
    CapturedFrame quietReference;
    bool haveFrame = false;
    bool responded = false;
    auto quietSince = start;
    const auto view = [](const CapturedFrame& f) {
        return f.View();
    };

    while (true) {
        auto wakeAt = deadline;
        if (haveFrame && responded) {
            wakeAt = std::min(deadline, quietSince + quietPeriod);
        }
        const double remainMs = std::max(0.0,
            std::chrono::duration<double, std::milli>(wakeAt - Clock::now()).count());
        CapturedFrame cur;
        if (WaitForFrameReference(lastSeen, cur, remainMs)) {
            lastSeen = cur.frameId;
            if (cur.displayTime > baseline.notBefore) {
                // 与固定参考帧比较，不能只比较相邻帧，否则缓慢动画会累积漂移。
                if (!haveFrame || !IsFrameStable(view(quietReference), view(cur), m_config.stability)) {
                    quietReference = cur;
                    quietSince = Clock::now();
                }
                responded = responded || (baseline.frame.View().data &&
                    !IsFrameStable(view(baseline.frame), view(cur), m_config.stability));
                latest = std::move(cur);
                haveFrame = true;
            }
        }
        {
            std::lock_guard<std::mutex> lock(m_frameMutex);
            if (m_streamFailed) return false;
            // 超时边缘如果已有新样本，先处理它，不能把上一个画面提前提交。
            if (m_latestFrameId > lastSeen && Clock::now() < deadline) continue;
        }
        const auto now = Clock::now();
        const bool quiet = haveFrame && now - quietSince >= quietPeriod;
        if (haveFrame && ((responded && quiet) || now >= deadline)) {
            // 未见响应时等满窗口才可判无变化；没有任何新样本不能拿缓存冒充成功。
            latest.stable = quiet;
            latest.pixels = *latest.sharedPixels;
            latest.sharedPixels.reset();
            out = std::move(latest);
            return true;
        }
        if (now >= deadline) return false;
    }
}

}  // namespace shotlet

#else  // __MAC_OS_X_VERSION_MAX_ALLOWED < 12300

namespace shotlet {

MacFrameSource::~MacFrameSource() {
    Stop();
}
bool MacFrameSource::Start(uint32_t, const GlobalRect&, bool, const std::function<bool()>&) {
    m_lastError = "SDK too old for ScreenCaptureKit";
    return false;
}
void MacFrameSource::Stop() {}
void MacFrameSource::RequestStop() {}
bool MacFrameSource::IsRunning() const { return false; }
uint64_t MacFrameSource::CurrentFrameId() const { return 0; }
CaptureBaseline MacFrameSource::TakeBaseline() const { return {}; }
bool MacFrameSource::WaitForNextFrame(uint64_t, CapturedFrame&, double) { return false; }
bool MacFrameSource::WaitForFrameReference(uint64_t, CapturedFrame&, double) { return false; }
bool MacFrameSource::WaitForStableFrame(const CaptureBaseline&, CapturedFrame&) { return false; }
void MacFrameSource::HandleFrame(const uint8_t*, int, int, int, bool, uint64_t) {}
void MacFrameSource::HandleIdleFrame(uint64_t) {}
void MacFrameSource::HandleStreamError(const char*) {}

}  // namespace shotlet

#endif  // __MAC_OS_X_VERSION_MAX_ALLOWED

#endif  // SHOTLET_PLATFORM_MACOS
