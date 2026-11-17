#include "ScreenOverlay.h"
#include "app/Application.h"
#include "core/Canvas.h"
#include "platform/TransparentWindow.h"
#include "platform/FileService.h"
#include "ui/Toolbar.h"
#include "utils/Logger.h"
#include "utils/CoordinateUtils.h"

#if defined(SHOTLET_PLATFORM_MACOS)

#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <algorithm>
#include <cmath>
#include <cstring>

// 自定义 NSView，用于渲染截图底图
@interface ScreenOverlayBackgroundView : NSView {
@public
    CGImageRef backgroundImage;  // 底图
}
@end

@implementation ScreenOverlayBackgroundView

- (id)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        backgroundImage = nil;
        self.wantsLayer = YES;
        self.layerContentsRedrawPolicy = NSViewLayerContentsRedrawOnSetNeedsDisplay;
    }
    return self;
}

- (void)drawRect:(NSRect)dirtyRect {
    [super drawRect:dirtyRect];

    if (!backgroundImage) return;

    CGContextRef ctx = (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
    CGContextDrawImage(ctx, self.bounds, backgroundImage);
}

- (void)dealloc {
    if (backgroundImage) {
        CGImageRelease(backgroundImage);
        backgroundImage = nil;
    }
}

@end

// 自定义 NSWindow，仅用于显示底图，不参与交互
@interface ScreenOverlayWindow : NSWindow
@end

@implementation ScreenOverlayWindow
- (BOOL)canBecomeKeyWindow { return NO; }
- (BOOL)canBecomeMainWindow { return NO; }
@end

namespace shotlet {

namespace {
    CGFloat AppKitWindowYForCGTopY(CGFloat cgTopY, CGFloat windowHeight) {
        for (NSScreen* screen in [NSScreen screens]) {
            NSDictionary* desc = [screen deviceDescription];
            NSNumber* screenNumber = desc[@"NSScreenNumber"];
            if (!screenNumber) continue;

            CGDirectDisplayID displayID = static_cast<CGDirectDisplayID>([screenNumber unsignedIntValue]);
            CGRect bounds = CGDisplayBounds(displayID);
            if (cgTopY >= bounds.origin.y && cgTopY < bounds.origin.y + bounds.size.height) {
                NSRect frame = screen.frame;
                return static_cast<CGFloat>(shotlet::CGYToAppKitBottomYInScreen(
                    cgTopY,
                    windowHeight,
                    frame.origin.y,
                    frame.size.height,
                    bounds.origin.y));
            }
        }

        CGFloat globalHeight = 0;
        for (NSScreen* screen in [NSScreen screens]) {
            CGFloat top = screen.frame.origin.y + screen.frame.size.height;
            if (top > globalHeight) globalHeight = top;
        }
        return globalHeight - cgTopY - windowHeight;
    }
}

bool ScreenOverlay::CreateNativeWindow() {
    // 计算窗口 frame
    // Config 中的 windowX/windowY 是 CGDisplayBounds 返回的全局坐标（左上角原点）
    // 需要转换为 AppKit 坐标（左下角原点）
    
    CGFloat appKitX = m_config.windowX;
    CGFloat appKitY = AppKitWindowYForCGTopY(m_config.windowY, m_config.windowHeight);

    NSRect windowRect = NSMakeRect(appKitX, appKitY,
                                    m_config.windowWidth, m_config.windowHeight);

    ScreenOverlayWindow* window = [[ScreenOverlayWindow alloc]
        initWithContentRect:windowRect
        styleMask:NSWindowStyleMaskBorderless
        backing:NSBackingStoreBuffered
        defer:NO];

    if (!window) {
        Logger::Error("[ScreenOverlay:mac] Failed to create NSWindow");
        return false;
    }
    
    // 关键：设置窗口层级高于菜单栏，但低于 SDL 窗口
    // NSWindow 仅用于显示底图，不接收任何输入事件
    [window setLevel:NSMainMenuWindowLevel + 1];
    [window setIgnoresMouseEvents:YES];  // 所有点击穿透到 SDL 窗口

    // 透明背景
    [window setOpaque:NO];
    [window setBackgroundColor:[NSColor clearColor]];
    [window setHasShadow:NO];
    // 底图覆盖整个屏幕，系统的窗口缩放动画会表现为桌面短暂缩小。
    [window setAnimationBehavior:NSWindowAnimationBehaviorNone];
    // setIgnoresMouseEvents 已在上面设置（YES），不要覆盖
    [window setCollectionBehavior:
        NSWindowCollectionBehaviorCanJoinAllSpaces |
        NSWindowCollectionBehaviorStationary |
        NSWindowCollectionBehaviorIgnoresCycle];
    [window setReleasedWhenClosed:NO];
    
    // 创建 background view
    ScreenOverlayBackgroundView* view = [[ScreenOverlayBackgroundView alloc]
        initWithFrame:NSMakeRect(0, 0, m_config.windowWidth, m_config.windowHeight)];
    [window setContentView:view];
    
    // 暂不显示，等底图设置好且首帧渲染完成后再显示
    // [window orderFront:nil];
    [window setAlphaValue:0.0];  // 完全透明，等准备好再显示
    
    m_nativeWindow = (__bridge_retained void*)window;

    return true;
}

void ScreenOverlay::DestroyNativeWindow() {
    if (!m_nativeWindow) return;

    ScreenOverlayWindow* window = (__bridge_transfer ScreenOverlayWindow*)m_nativeWindow;
    [window close];
    m_nativeWindow = nullptr;
}

void ScreenOverlay::UpdateNativeBackground(const unsigned char* imageData,
                                             int pixelWidth, int pixelHeight, int rowPitch) {
    if (!m_nativeWindow || !imageData) return;
    
    ScreenOverlayWindow* window = (__bridge ScreenOverlayWindow*)m_nativeWindow;
    ScreenOverlayBackgroundView* view = (ScreenOverlayBackgroundView*)window.contentView;
    if (!view) return;
    
    // 从 RGBA 数据创建 CGImage
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    CGBitmapInfo bitmapInfo = (CGBitmapInfo)kCGImageAlphaPremultipliedLast;
    
    CGContextRef bitmapCtx = CGBitmapContextCreate(
        const_cast<unsigned char*>(imageData), pixelWidth, pixelHeight, 8, rowPitch, colorSpace, bitmapInfo);
    
    if (!bitmapCtx) {
        Logger::Error("[ScreenOverlay:mac] CGBitmapContextCreate failed ({}x{} pitch={})",
                      pixelWidth, pixelHeight, rowPitch);
        CGColorSpaceRelease(colorSpace);
        return;
    }
    
    CGImageRef cgImage = CGBitmapContextCreateImage(bitmapCtx);
    CGContextRelease(bitmapCtx);
    CGColorSpaceRelease(colorSpace);
    
    if (!cgImage) {
        Logger::Error("[ScreenOverlay:mac] CGBitmapContextCreateImage failed");
        return;
    }
    
    // 设置底图
    if (view->backgroundImage) {
        CGImageRelease(view->backgroundImage);
    }
    view->backgroundImage = cgImage;
    [view setNeedsDisplay:YES];

    // 不在这里 orderFront，等首帧渲染后再显示（避免白闪）
}

void ScreenOverlay::ShowNativeWindow() {
    if (!m_nativeWindow) return;

    ScreenOverlayWindow* window = (__bridge ScreenOverlayWindow*)m_nativeWindow;
    [window setAlphaValue:1.0];  // 从透明变为可见
    [window orderFront:nil];

    // 确保 SDL 窗口保持键盘焦点（NSWindow 不可成为 key/main）
    if (m_sdlWindow) {
        SDL_PropertiesID props = SDL_GetWindowProperties(m_sdlWindow);
        NSWindow* sdlNsWindow = (__bridge NSWindow*)SDL_GetPointerProperty(
            props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        if (sdlNsWindow) {
            [sdlNsWindow makeKeyWindow];
        }
    }
}

void ScreenOverlay::HideNativeWindow() {
    if (!m_nativeWindow) return;

    ScreenOverlayWindow* window = (__bridge ScreenOverlayWindow*)m_nativeWindow;
    [window setAlphaValue:0.0];
    [window orderOut:nil];
}

void ScreenOverlay::ClearNativeBackground() {
    if (!m_nativeWindow) return;

    ScreenOverlayWindow* window = (__bridge ScreenOverlayWindow*)m_nativeWindow;
    ScreenOverlayBackgroundView* view = (ScreenOverlayBackgroundView*)window.contentView;
    if (!view) return;

    if (view->backgroundImage) {
        CGImageRelease(view->backgroundImage);
        view->backgroundImage = nil;
    }
    [view setNeedsDisplay:YES];
}

void ScreenOverlay::HideSDLWindow() {
    if (!m_sdlWindow) return;

    SDL_PropertiesID props = SDL_GetWindowProperties(m_sdlWindow);
    NSWindow* nsWindow = (__bridge NSWindow*)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (!nsWindow) return;

    // 隐藏窗口：alpha=0 且鼠标穿透，不干扰用户操作
    [nsWindow setAlphaValue:0.0];
    [nsWindow setIgnoresMouseEvents:YES];
    // orderOut 把窗口移出 WindowServer 屏幕合成：仅 alpha=0 时窗口仍在
    // 合成列表中，WindowServer/FramePacing 会持有其 CAMetalLayer 已呈现
    // 的最后一帧 drawable——layer 销毁后引用也不释放，reclaim 模式下每轮
    // 截图泄漏一整组全尺寸 drawable（双屏约 160MB）。RevealSDLWindow 的
    // makeKeyAndOrderFront 与此对称。
    [nsWindow orderOut:nil];
}

void ScreenOverlay::ApplyGpuBufferCount() {
    int count = Application::Instance() ? Application::Instance()->GetGpuBufferCount() : 2;
    if (count != 2 && count != 3) return;
    if (!m_sdlWindow) return;

    SDL_PropertiesID props = SDL_GetWindowProperties(m_sdlWindow);
    NSWindow* nsWindow = (__bridge NSWindow*)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (!nsWindow) return;

    // SDL GPU Metal 后端在 claim 时把 SDL3_cocoametalview 挂为 contentView 的
    // 子视图（SDL_cocoametalview.m），CAMetalLayer 是该子视图的 backing layer
    for (NSView* subview in nsWindow.contentView.subviews) {
        CALayer* layer = subview.layer;
        if ([layer isKindOfClass:[CAMetalLayer class]]) {
            CAMetalLayer* metalLayer = (CAMetalLayer*)layer;
            if ((int)metalLayer.maximumDrawableCount != count) {
                metalLayer.maximumDrawableCount = (NSUInteger)count;
            }
            Logger::Info("[ScreenOverlay:mac] swapchain maximumDrawableCount={} display={}",
                         count, m_config.displayId);
            return;
        }
    }
    Logger::Warning("[ScreenOverlay:mac] CAMetalLayer not found, gpu-buffers={} not applied display={}",
                    count, m_config.displayId);
}

void ScreenOverlay::RevealSDLWindow() {
    if (!m_sdlWindow) return;

    SDL_PropertiesID props = SDL_GetWindowProperties(m_sdlWindow);
    NSWindow* nsWindow = (__bridge NSWindow*)SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (!nsWindow) return;

    // 显示并激活 SDL 窗口
    [nsWindow setAlphaValue:1.0];
    [nsWindow setIgnoresMouseEvents:NO];
    [nsWindow makeKeyAndOrderFront:nil];
    [nsWindow makeKeyWindow];
    [nsWindow makeMainWindow];
}

} // namespace shotlet

#endif // SHOTLET_PLATFORM_MACOS
