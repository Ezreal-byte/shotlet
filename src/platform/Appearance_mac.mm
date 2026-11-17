#include "Appearance.h"
#include <SDL3/SDL.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
namespace shotlet {
std::optional<Theme> QuerySystemTheme() {
    NSAppearanceName name = [NSApp.effectiveAppearance bestMatchFromAppearancesWithNames:
        @[NSAppearanceNameAqua, NSAppearanceNameDarkAqua]];
    if (!name) return {};
    return [name isEqualToString:NSAppearanceNameDarkAqua] ? Theme::Dark : Theme::Light;
}
void ApplyNativeWindowTheme(void* window, Theme theme) {
    NSWindow* native = (__bridge NSWindow*)window;
    native.appearance = [NSAppearance appearanceNamed:theme == Theme::Dark ? NSAppearanceNameDarkAqua : NSAppearanceNameAqua];
}
std::optional<Theme> QuerySystemTrayTheme() { return QuerySystemTheme(); }
void ApplyWindowTheme(SDL_Window* window, Theme theme) {
    if (!window) return;
    ApplyNativeWindowTheme(SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr), theme);
}
void ApplyPopupWindowCorners(SDL_Window* window, float radiusWindowUnits) {
    if (!window) return;
    NSWindow* native = (__bridge NSWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
        SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
    if (!native) return;
    // 背景透明和原生图层裁剪必须同时设置，避免 Metal 窗口的圆角外侧露出黑底。
    native.opaque = NO;
    native.backgroundColor = NSColor.clearColor;
    native.hasShadow = YES;
    native.contentView.wantsLayer = YES;
    native.contentView.layer.opaque = NO;
    native.contentView.layer.cornerRadius = radiusWindowUnits;
    native.contentView.layer.masksToBounds = YES;
    [native invalidateShadow];
}
} // namespace shotlet
