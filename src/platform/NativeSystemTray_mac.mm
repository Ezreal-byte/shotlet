#include "NativeSystemTray.h"
#include "platform/MacImageOutput.h"
#include <SDL3/SDL.h>
#include <memory>
#import <AppKit/AppKit.h>

@interface ShotletTrayTarget : NSObject
@property(nonatomic) shotlet::TrayActionCallback callback;
@property(nonatomic) void* context;
@property(nonatomic, strong) NSStatusItem* item;
@property(nonatomic, strong) NSMenu* menu;
- (void)clicked:(id)sender;
- (void)perform:(NSMenuItem*)sender;
@end

@implementation ShotletTrayTarget
- (void)clicked:(id)sender {
    if (NSApp.currentEvent.type == NSEventTypeRightMouseUp ||
        (NSApp.currentEvent.modifierFlags & NSEventModifierFlagControl)) {
        // 临时挂菜单保留系统原生菜单体验；左键始终直接打开最近截图。
        self.item.menu = self.menu;
        [self.item.button performClick:nil];
        self.item.menu = nil;
        return;
    }
    self.callback(self.context, shotlet::TrayAction::Recent);
}
- (void)perform:(NSMenuItem*)sender {
    self.callback(self.context, static_cast<shotlet::TrayAction>(sender.tag));
}
@end

namespace shotlet {
namespace {
constexpr CGFloat kMenuBarIconLogicalSize = 22;

NSImage* TrayImage(SDL_Surface* surface) {
    if (!surface) return nil;
    CGImageRef image = CreateOwnedImageFromRGBA(static_cast<const unsigned char*>(surface->pixels),
        surface->w, surface->h, surface->pitch);
    if (!image) return nil;
    NSImage* result = [[NSImage alloc] initWithCGImage:image size:NSMakeSize(kMenuBarIconLogicalSize, kMenuBarIconLogicalSize)];
    CGImageRelease(image);
    return result;
}
} // namespace

struct NativeSystemTray { ShotletTrayTarget* __strong target; };
struct PopupFocusToken { NSRunningApplication* __strong application; };

NativeSystemTray* CreateNativeSystemTray(SDL_Surface* icon, Language language, TrayActionCallback callback, void* data) {
    NSImage* image = TrayImage(icon);
    if (!image) return nullptr;
    auto tray = std::make_unique<NativeSystemTray>();
    tray->target = [ShotletTrayTarget new];
    auto target = tray->target;
    target.callback = callback;
    target.context = data;
    target.item = [NSStatusBar.systemStatusBar statusItemWithLength:NSSquareStatusItemLength];
    if (!target.item) return nullptr;
    target.item.button.image = image;
    target.item.button.toolTip = [NSString stringWithUTF8String:Tr(language, TextId::TrayTooltip)];
    target.item.button.target = target;
    target.item.button.action = @selector(clicked:);
    [target.item.button sendActionOn:NSEventMaskLeftMouseUp | NSEventMaskRightMouseUp];
    target.menu = [NSMenu new];
    const std::pair<TextId, TrayAction> entries[] = {
        {TextId::RecentCapturesTitle, TrayAction::Recent}, {TextId::CopyLatestCapture, TrayAction::CopyLatest},
        {TextId::HistoryTitle, TrayAction::History}, {TextId::SettingsTitle, TrayAction::Settings},
        {TextId::TrayQuit, TrayAction::Quit}};
    for (const auto& [label, action] : entries) {
        if (action == TrayAction::Settings) [target.menu addItem:NSMenuItem.separatorItem];
        NSMenuItem* item = [[NSMenuItem alloc] initWithTitle:[NSString stringWithUTF8String:Tr(language, label)]
                                                   action:@selector(perform:) keyEquivalent:@""];
        item.target = target;
        item.tag = static_cast<NSInteger>(action);
        [target.menu addItem:item];
    }
    return tray.release();
}

void DestroyNativeSystemTray(NativeSystemTray* tray) {
    if (!tray) return;
    [NSStatusBar.systemStatusBar removeStatusItem:tray->target.item];
    tray->target.item.button.target = nil;
    delete tray;
}

void UpdateNativeSystemTrayIcon(NativeSystemTray* tray, SDL_Surface* icon) {
    if (!tray) return;
    NSImage* image = TrayImage(icon);
    // 新图像创建失败时保留有效的旧图标，不能把状态栏图像替换为空。
    if (image) tray->target.item.button.image = image;
}

SDL_Rect NativeSystemTrayAnchor(NativeSystemTray* tray) {
    if (!tray) return {};
    NSButton* button = tray->target.item.button;
    const NSRect frame = [button.window convertRectToScreen:[button convertRect:button.bounds toView:nil]];
    // SDL 使用主显示器左上原点；AppKit 的 Y 轴向上，不能再乘 Retina 像素密度。
    const CGFloat primaryHeight = CGDisplayBounds(CGMainDisplayID()).size.height;
    return {int(NSMinX(frame)), int(primaryHeight - NSMaxY(frame)), int(frame.size.width), int(frame.size.height)};
}

PopupFocusToken* RememberPopupFocus() {
    return new PopupFocusToken{NSWorkspace.sharedWorkspace.frontmostApplication};
}

void ReleasePopupFocus(PopupFocusToken* token, SDL_Window*, bool restore) {
    if (!token) return;
    if (restore && NSApp.isActive && token->application && !token->application.terminated &&
        token->application.processIdentifier != NSProcessInfo.processInfo.processIdentifier) {
        // 自身仍在前台时使用默认激活选项恢复原应用，无需已弃用的强制激活标志。
        [token->application activateWithOptions:NSApplicationActivationOptions{}];
    }
    delete token;
}
} // namespace shotlet
