#include "platform/ImageEditorWindow.h"
#include "utils/Logger.h"
#include "ui/Toolbar.h"
#include "ui/ImageEditorLayout.h"
#include <SDL3/SDL.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <algorithm>
#include <cmath>

namespace shotlet {
namespace {
namespace EditorWindowLayout {
    constexpr double NativeControlsGap = 16;
    constexpr double SizeTolerance = 1;
}
NSWindow* Native(SDL_Window* window) {
    return (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
}
}
void SetupHistoryBrowserWindow(SDL_Window* window, Theme theme, bool closeOnly) {
    NSWindow* native = Native(window);
    if (!native) return;
    native.styleMask |= NSWindowStyleMaskFullSizeContentView;
    native.titleVisibility = NSWindowTitleHidden;
    native.titlebarAppearsTransparent = YES;
    native.movableByWindowBackground = NO;
    native.animationBehavior = NSWindowAnimationBehaviorNone;
    if (closeOnly) {
        // 仅保留关闭按钮：隐藏最小化（黄）与最大化（绿）。
        for (NSWindowButton type : {NSWindowMiniaturizeButton, NSWindowZoomButton}) {
            [[native standardWindowButton:type] setHidden:YES];
        }
    }
    const auto background = ImGui::ColorConvertU32ToFloat4(GetHistoryPalette(theme).sidebar);
    native.backgroundColor = [NSColor colorWithSRGBRed:background.x green:background.y blue:background.z alpha:background.w];
    SDL_SyncWindow(window);
    LayoutHistoryBrowserWindow(window, 52);
}
void HideHistoryBrowserWindow(SDL_Window* window) {
    if (!window) return;
    // 先让窗口完全透明，再移出合成列表；显式提交更新后才允许开始取屏。
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    SDL_SetWindowOpacity(window, 0);
    SDL_HideWindow(window);
    SDL_SyncWindow(window);
    [CATransaction commit];
    [CATransaction flush];
}
namespace {
void LayoutWindowTitlebar(SDL_Window* window, float headerHeight) {
    NSWindow* native = Native(window);
    if (!native || !native.contentView || (native.styleMask & NSWindowStyleMaskFullScreen)) return;
    NSButton* close = [native standardWindowButton:NSWindowCloseButton];
    NSView* container = close.superview.superview;
    if (!container.superview) return;
    // 同步扩大原生标题栏容器，确保下移后的按钮仍可显示和点击。
    NSRect frame = container.frame;
    frame.size.height = headerHeight;
    frame.origin.y = container.superview.isFlipped ? NSMinY(container.superview.bounds)
        : NSMaxY(container.superview.bounds) - headerHeight;
    if (!NSEqualRects(container.frame, frame)) container.frame = frame;
    const NSRect bounds = native.contentView.bounds;
    const NSPoint center = NSMakePoint(NSMinX(bounds), native.contentView.isFlipped
        ? NSMinY(bounds) + headerHeight * .5 : NSMaxY(bounds) - headerHeight * .5);
    for (NSWindowButton type : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
        NSButton* button = [native standardWindowButton:type];
        if (!button.superview) continue;
        const NSPoint localCenter = [button.superview convertPoint:center fromView:native.contentView];
        NSPoint origin = button.frame.origin;
        origin.y = localCenter.y - NSHeight(button.frame) * .5;
        if (!NSEqualPoints(button.frame.origin, origin)) [button setFrameOrigin:origin];
    }
}
} // namespace
void LayoutHistoryBrowserWindow(SDL_Window* window, float headerHeight) {
    LayoutWindowTitlebar(window, headerHeight);
}
float LayoutImageEditorTitlebar(SDL_Window* window, float toolbarHeight) {
    NSWindow* native = Native(window);
    if (!native || !native.contentView || (native.styleMask & NSWindowStyleMaskFullScreen)) return 0;
    LayoutWindowTitlebar(window, toolbarHeight);
    double controlsRight = 0;
    for (NSWindowButton type : {NSWindowCloseButton, NSWindowMiniaturizeButton, NSWindowZoomButton}) {
        NSButton* button = [native standardWindowButton:type];
        if (!button || !button.superview || button.hidden) continue;
        const NSRect frame = [button convertRect:button.bounds toView:native.contentView];
        controlsRight = std::max(controlsRight, NSMaxX(frame));
    }
    return controlsRight > 0 ? controlsRight + EditorWindowLayout::NativeControlsGap : 0;
}
bool SetupImageEditorWindow(SDL_Window* window, int imageWidth, int imageHeight, Language language) {
    if (!window || imageWidth <= 0 || imageHeight <= 0) return false;
    NSWindow* native = Native(window);
    if (!native) return false;
    // 窗口已定位到来源屏幕，native.screen 即目标屏幕
    NSScreen* screen = native.screen ?: NSScreen.mainScreen;
    if (!screen) return false;
    // 内容延伸到标题栏，继续使用同一份 ImGui 图标，避免原生图标替换后形状和线宽变化。
    native.styleMask |= NSWindowStyleMaskFullSizeContentView;
    native.titleVisibility = NSWindowTitleHidden;
    const NSRect visible = screen.visibleFrame;
    // 宽度保持有界，长图高度按适应宽度后的内容计算，最多占满屏幕可用区并留边。
    const double backingScale = std::max(1.0, double(screen.backingScaleFactor));
    const auto size = ImageEditorInitialWindowSize(ImVec2(float(imageWidth), float(imageHeight)),
        ImVec2(float(visible.size.width), float(visible.size.height)), float(backingScale));
    const NSSize targetFrame = NSMakeSize(size.x, size.y);
    // FullSizeContentView 让标题栏包含在内容区内，不能再额外叠加一条标题栏高度。
    NSRect content = [native contentRectForFrameRect:NSMakeRect(0, 0, targetFrame.width, targetFrame.height)];
    SDL_SetWindowMinimumSize(window, std::min(int(ImageEditorLayout::MinimumWindowWidth), int(content.size.width)),
        std::min(int(ImageEditorLayout::MinimumWindowHeight), int(content.size.height)));
    SDL_SetWindowTitle(window, Tr(language, TextId::EditorTitle));
    if (!SDL_SetWindowSize(window, int(content.size.width), int(content.size.height))) return false;
    // AppKit（左下原点）与 SDL（左上原点）坐标系不同，统一用 AppKit 直接
    // 定位外框居中于目标屏可视区，避免跨坐标系换算误差（多屏下尤其明显）
    [native setFrameOrigin:NSMakePoint(NSMidX(visible) - native.frame.size.width / 2,
                                       NSMidY(visible) - native.frame.size.height / 2)];
    SDL_SyncWindow(window);
    native.level = NSNormalWindowLevel;
    native.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary;
    native.opaque = YES;
    // 背景色与渲染 clear color 一致，规避显示后首帧白闪
    const auto background = Colors::EditorInitialBackgroundVec4;
    native.backgroundColor = [NSColor colorWithWhite:background.x alpha:background.w];
    native.hasShadow = YES;
    native.titlebarAppearsTransparent = YES;
    native.movableByWindowBackground = NO;
    native.ignoresMouseEvents = NO;
    native.alphaValue = 1;
    LayoutImageEditorTitlebar(window, Toolbar::EditorHeight);
    // 对照 SDL 内容区目标校验 AppKit 实际尺寸，防止样式切换后窗口尺寸不同步。
    const NSRect actualContent = [native contentRectForFrameRect:native.frame];
    if (std::abs(actualContent.size.width - content.size.width) > EditorWindowLayout::SizeTolerance ||
        std::abs(actualContent.size.height - content.size.height) > EditorWindowLayout::SizeTolerance) {
        Logger::Error("[ImageEditor] setup resize failed: target={}x{}, actual={}x{}",
            content.size.width, content.size.height, actualContent.size.width, actualContent.size.height);
        return false;
    }
    Logger::Info("[ImageEditor] window={}x{}pt / {}x{}px, backingScale={}, origin=({},{}), image={}x{}px",
        native.frame.size.width, native.frame.size.height, native.frame.size.width * backingScale,
        native.frame.size.height * backingScale, backingScale, native.frame.origin.x, native.frame.origin.y, imageWidth, imageHeight);
    return true;
}

void ApplyEditorBackground(SDL_Window* window, Theme theme) {
    if (!window) return;
    NSWindow* native = (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    const auto color = ImGui::ColorConvertU32ToFloat4(GetThemePalette(theme).editorBackground);
    native.backgroundColor = [NSColor colorWithSRGBRed:color.x green:color.y blue:color.z alpha:color.w];
}

void ApplyEditorGpuBufferCount(SDL_Window* window, int count) {
    if (count != 2 && count != 3) return;
    if (!window) return;

    SDL_PropertiesID props = SDL_GetWindowProperties(window);
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
            Logger::Info("[ImageEditor] swapchain maximumDrawableCount={}", count);
            return;
        }
    }
    Logger::Warning("[ImageEditor] CAMetalLayer not found, gpu-buffers={} not applied", count);
}
} // namespace shotlet
