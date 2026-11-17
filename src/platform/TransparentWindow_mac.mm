#include "TransparentWindow.h"
#include "utils/Logger.h"

#if defined(SHOTLET_PLATFORM_MACOS)

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <SDL3/SDL.h>

namespace shotlet {

void ConfigureWindowTransparent(SDL_Window* window) {
    if (!window) return;

    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    NSWindow* nsWindow = (__bridge NSWindow*)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);

    if (!nsWindow) {
        Logger::Warning("Failed to get NSWindow from SDL window");
        return;
    }

    // 设置透明属性（不调用 makeKeyAndOrderFront）
    [nsWindow setOpaque:NO];
    [nsWindow setBackgroundColor:[NSColor clearColor]];
    [nsWindow setHasShadow:NO];
    // SDL_GPU 不接受 SDL_WINDOW_TRANSPARENT，Metal 子图层须随原生窗口一起配置。
    // 每次重新 claim 都会创建新图层，因此调用方需在复用交换链后再次配置。
    for (NSView* subview in nsWindow.contentView.subviews) {
        if ([subview.layer isKindOfClass:CAMetalLayer.class]) subview.layer.opaque = NO;
    }
    // 截图交互层应直接显示，避免 orderFront/orderOut 的系统缩放动画。
    [nsWindow setAnimationBehavior:NSWindowAnimationBehaviorNone];

    // 确保窗口接收鼠标事件
    [nsWindow setIgnoresMouseEvents:NO];
    [nsWindow setAcceptsMouseMovedEvents:YES];

    // 窗口层级高于 ScreenOverlay 的底图 NSWindow (NSMainMenuWindowLevel+1)
    [nsWindow setLevel:NSMainMenuWindowLevel + 2];
    [nsWindow setCollectionBehavior:NSWindowCollectionBehaviorCanJoinAllSpaces |
                                    NSWindowCollectionBehaviorStationary |
                                    NSWindowCollectionBehaviorIgnoresCycle];
}

void SetWindowTransparent(SDL_Window* window) {
    if (!window) return;

    ConfigureWindowTransparent(window);

    // 显示窗口
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    NSWindow* nsWindow = (__bridge NSWindow*)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (!nsWindow) return;

    [nsWindow makeKeyAndOrderFront:nil];
    [nsWindow makeKeyWindow];
    [nsWindow makeMainWindow];
}

void SetWindowOpaque(SDL_Window* window) {
    if (!window) return;
    
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    NSWindow* nsWindow = (__bridge NSWindow*)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    
    if (!nsWindow) return;
    
    [nsWindow setOpaque:YES];
    [nsWindow setBackgroundColor:[NSColor windowBackgroundColor]];
    [nsWindow setHasShadow:YES];
}

void ForceWindowFocus(SDL_Window* window) {
    if (!window) return;

    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    NSWindow* nsWindow = (__bridge NSWindow*)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);

    if (!nsWindow) {
        return;
    }

    // The app bundle is configured with LSUIElement=true, so keep the process
    // in Accessory mode. Switching to Regular overrides LSUIElement and causes
    // a transient Dock icon when Electron launches the helper.
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    [NSApp activateIgnoringOtherApps:YES];

    // [NSApp activateIgnoringOtherApps:] 是异步的，需要处理 Cocoa 事件才能生效。
    NSDate* timeout = [NSDate dateWithTimeIntervalSinceNow:1.0];
    while (!NSApp.isActive && [[NSDate date] compare:timeout] == NSOrderedAscending) {
        NSEvent* event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                        untilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]
                                           inMode:NSDefaultRunLoopMode
                                          dequeue:YES];
        if (event) {
            [NSApp sendEvent:event];
        }
    }

    // 强制获取键盘焦点
    [nsWindow makeKeyAndOrderFront:nil];
    [nsWindow makeKeyWindow];
    [nsWindow makeMainWindow];
}

bool RestoreActivationPolicy() {
    [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    return true;
}

} // namespace shotlet

#endif // SHOTLET_PLATFORM_MACOS
