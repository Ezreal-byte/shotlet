#pragma once
#include "native_events_mac.h"
#include "utils/FilePathUtils.h"
#include <cmath>
#import <ScreenCaptureKit/ScreenCaptureKit.h>

namespace shotlet::TestUtils {
inline NSWindow* FindNativeWindow(NSString* identifier) {
    for (NSWindow* window in NSApp.windows) if ([window.identifier isEqualToString:identifier] && window.visible) return window;
    return nil;
}
template<class View> View* FindNativeView(NSWindow* window, Class type) {
    for (NSView* view in window.contentView.subviews) if ([view isKindOfClass:type]) return static_cast<View*>(view);
    return nil;
}
// 读取 WindowServer 合成结果；NSView 的离屏缓存无法包含 AVPlayer 的硬件视频层。
inline bool SaveNativeWindowPreview(NSWindow* window, const std::filesystem::path& path, std::string& error) {
    if (@available(macOS 14.0, *)) {
        __block SCShareableContent* content = nil;
        __block NSError* failure = nil;
        auto ready = dispatch_semaphore_create(0);
        [SCShareableContent getShareableContentWithCompletionHandler:^(SCShareableContent* value, NSError* reason) {
            content = value; failure = reason; dispatch_semaphore_signal(ready);
        }];
        if (!PumpUntil([&] { return dispatch_semaphore_wait(ready, DISPATCH_TIME_NOW) == 0; })) {
            error = "获取窗口信息超时"; return false;
        }
        SCWindow* source = nil;
        for (SCWindow* candidate in content.windows) if (candidate.windowID == window.windowNumber) { source = candidate; break; }
        if (!source) { error = failure.localizedDescription.UTF8String ?: "未找到预览窗口"; return false; }
        auto* configuration = [[SCStreamConfiguration alloc] init];
        configuration.width = size_t(std::lround(source.frame.size.width * window.backingScaleFactor));
        configuration.height = size_t(std::lround(source.frame.size.height * window.backingScaleFactor));
        configuration.showsCursor = NO;
        configuration.ignoreShadowsSingleWindow = YES;
        configuration.colorSpaceName = kCGColorSpaceSRGB;
        auto* filter = [[SCContentFilter alloc] initWithDesktopIndependentWindow:source];
        __block NSBitmapImageRep* bitmap = nil;
        [SCScreenshotManager captureImageWithFilter:filter configuration:configuration completionHandler:^(CGImageRef image, NSError* reason) {
            if (image) bitmap = [[NSBitmapImageRep alloc] initWithCGImage:image];
            failure = reason; dispatch_semaphore_signal(ready);
        }];
        if (!PumpUntil([&] { return dispatch_semaphore_wait(ready, DISPATCH_TIME_NOW) == 0; })) {
            error = "读取窗口画面超时"; return false;
        }
        if (!bitmap) { error = failure.localizedDescription.UTF8String ?: "读取窗口画面失败"; return false; }
        const auto utf8 = FilePathUtils::ToUtf8(path);
        NSData* png = [bitmap representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
        return [png writeToFile:[NSString stringWithUTF8String:utf8.c_str()] atomically:YES];
    }
    error = "窗口截图测试需要 macOS 14 或更新版本"; return false;
}
}
