#include "platform/LocalizedText.h"
#include "FileService.h"
#include "MacImageOutput.h"
#include <future>
#include <chrono>
#include "utils/Logger.h"

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace shotlet {

std::filesystem::path ChooseExportDirectory(Language language, TextId title) {
    NSOpenPanel* panel = [NSOpenPanel openPanel];
    panel.canChooseDirectories = YES; panel.canChooseFiles = NO;
    panel.canCreateDirectories = YES; panel.allowsMultipleSelection = NO;
    panel.title = TrNative(language,title);panel.prompt = TrNative(language,TextId::HistoryConfirm);
    [NSApp activateIgnoringOtherApps:YES];
    return [panel runModal] == NSModalResponseOK ? std::filesystem::path(panel.URL.path.UTF8String) : std::filesystem::path();
}
bool RevealInFileManager(const std::filesystem::path& file) {
    if (file.empty()) return false;
    [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[[NSURL fileURLWithPath:[NSString stringWithUTF8String:file.c_str()]]]];
    return true;
}
bool OpenFileWithDefaultApplication(const std::filesystem::path& file) {
    if (file.empty()) return false;
    NSString* path = [NSString stringWithUTF8String:file.c_str()];
    if (!path) return false;
    return [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:path]];
}
CaptureSourceInfo GetForegroundCaptureSource() {
    CaptureSourceInfo result;
    NSRunningApplication* application = NSWorkspace.sharedWorkspace.frontmostApplication;
    if (application.processIdentifier == NSProcessInfo.processInfo.processIdentifier) return result;
    result.application = application.localizedName.UTF8String ?: "";
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID);
    if (!list) return result;
    for (NSDictionary* info in (__bridge NSArray*)list) {
        if ([info[(id)kCGWindowOwnerPID] intValue] != application.processIdentifier || [info[(id)kCGWindowLayer] intValue] != 0) continue;
        result.window = [info[(id)kCGWindowName] UTF8String] ?: ""; break;
    }
    CFRelease(list); return result;
}

void ActivateAppForSaveDialog() {
    // macOS: 销毁 Overlay 后应用变为后台状态，NSSavePanel 无法获取键盘焦点
    // LSUIElement=true hides the Dock icon; keep Accessory mode when activating
    // so save dialogs do not make the helper appear in the Dock.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp activateIgnoringOtherApps:YES];
}

std::string SaveFileWithDialog(const unsigned char* rgbaData, int width, int height, Language language) {
    @autoreleasepool {
    if (!rgbaData || width <= 0 || height <= 0) {
        Logger::Error("SaveFileWithDialog: invalid parameters");
        return "";
    }

    // 确保 app 处于激活状态
    [NSApp activateIgnoringOtherApps:YES];

    // 使用 runModal 方式显示保存对话框
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.allowedContentTypes = @[[UTType typeWithFilenameExtension:@"png"]];
    panel.nameFieldStringValue = @"Screenshot.png";
    panel.canCreateDirectories = YES;
    [panel setPrompt:TrNative(language, TextId::SavePrompt)];
    [panel setMessage:TrNative(language, TextId::SaveLocation)];

    // 先隐藏 SDL 窗口，避免干扰
    NSWindow* sdlWindow = [NSApp keyWindow];
    BOOL windowWasHidden = NO;
    if (sdlWindow && sdlWindow.isVisible) {
        [sdlWindow orderOut:nil];
        windowWasHidden = YES;
    }

    NSInteger result = [panel runModal];

    // 恢复 SDL 窗口
    if (windowWasHidden && sdlWindow) {
        [sdlWindow makeKeyAndOrderFront:nil];
    }

    if (result == NSModalResponseOK) {
        NSURL* url = panel.URL;
        const std::string path(url.path.UTF8String);
        // 编码直接写临时文件，后台执行；主线程继续泵 AppKit 事件。
        auto save = std::async(std::launch::async, [rgbaData, width, height, path] {
            return WritePngToFile(rgbaData, width, height, path);
        });
        while (save.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready) {
            [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate date]];
        }
        if (save.get()) return path;
    }

    return "";
    }
}

} // namespace shotlet
