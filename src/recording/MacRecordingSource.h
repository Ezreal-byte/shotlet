#pragma once
#include "ScreenRecorder.h"
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#import <AppKit/AppKit.h>
#include <algorithm>

namespace shotlet::MacRecordingSource {
constexpr int CallbackTimeoutSeconds = 15;
inline bool Wait(dispatch_semaphore_t signal) {
    return dispatch_semaphore_wait(signal, dispatch_time(DISPATCH_TIME_NOW,
        int64_t(CallbackTimeoutSeconds) * NSEC_PER_SEC)) == 0;
}

inline bool Configure(const RecordingOptions& options, SCContentFilter* __strong& filter, SCStreamConfiguration* __strong& configuration,
                      std::string& error) API_AVAILABLE(macos(15.0)) {
#if __MAC_OS_X_VERSION_MAX_ALLOWED < 150000
    error = "录屏需要使用 macOS 15 或更新 SDK 构建"; return false;
#else
    __block SCShareableContent* content = nil;
    __block NSError* nativeError = nil;
    auto ready = dispatch_semaphore_create(0);
    [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* value, NSError* failure) {
        content = value; nativeError = failure; dispatch_semaphore_signal(ready);
    }];
    if (!Wait(ready)) { error = "获取屏幕信息超时"; return false; }
    if (!content) { error = nativeError.localizedDescription.UTF8String ?: "无法获取屏幕信息"; return false; }
    SCDisplay* display = nil;
    for (SCDisplay* candidate in content.displays)
        if (candidate.displayID == static_cast<uint32_t>(options.displayId)) { display = candidate; break; }
    if (!display) { error = "录制显示器已断开"; return false; }
    NSMutableArray<SCWindow*>* excludedWindows = [NSMutableArray array];
    for (SCWindow* window in content.windows)
        if (std::find(options.excludedWindowIds.begin(), options.excludedWindowIds.end(), window.windowID) != options.excludedWindowIds.end())
            [excludedWindows addObject:window];
    if (options.camera) {
        bool found = false;
        for (SCWindow* window in content.windows)
            if (window.windowID == options.cameraWindowId) { found = true; break; }
        if (!found) { error = "摄像头预览尚未就绪，请稍后重试"; return false; }
    }
    // 按窗口角色精确排除，不能排除整个宿主进程，否则摄像头和 Electron 主界面也会丢失。
    filter = [[SCContentFilter alloc] initWithDisplay:display excludingWindows:excludedWindows];
    configuration = [[SCStreamConfiguration alloc] init];
    const auto bounds = CGDisplayBounds(display.displayID);
    configuration.sourceRect = CGRectMake(options.region.x - bounds.origin.x, options.region.y - bounds.origin.y,
        options.region.width, options.region.height);
    configuration.width = options.widthPixels; configuration.height = options.heightPixels;
    const int fps = options.format == RecordingFormat::Gif ? GifFramesPerSecond : RecordingFramesPerSecond;
    configuration.minimumFrameInterval = CMTimeMake(1, fps);
    constexpr int Mp4CaptureQueueDepth = 8, GifCaptureQueueDepth = 3;
    constexpr int AudioSampleRateHz = 48000, AudioChannels = 2;
    // H.264 编码器会保留多帧 IOSurface；3 帧采集队列会与编码器互相等待，成片只留下前 4 帧。
    // MP4 使用系统支持的 8 帧录制缓冲，GIF 同步消费样本，可继续使用较小队列。
    configuration.queueDepth = options.format == RecordingFormat::Mp4 ? Mp4CaptureQueueDepth : GifCaptureQueueDepth;
    configuration.showsCursor = options.showCursor;
    configuration.capturesAudio = options.format == RecordingFormat::Mp4 && options.systemAudio;
    configuration.captureMicrophone = options.format == RecordingFormat::Mp4 && options.microphone;
    configuration.sampleRate = AudioSampleRateHz; configuration.channelCount = AudioChannels;
    configuration.captureDynamicRange = SCCaptureDynamicRangeSDR;
    // GIF 调色板按 sRGB 编码，统一采集色域，避免广色域屏幕的颜色在普通播放器中偏移。
    configuration.colorSpaceName = kCGColorSpaceSRGB;
    return true;
#endif
}
}
