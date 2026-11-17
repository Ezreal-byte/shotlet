#pragma once
#include "NumericUtils.h"
#include <SDL3/SDL_video.h>
#include <SDL3/SDL_rect.h>
#include <algorithm>

namespace shotlet::WindowUtils {

constexpr float DefaultScale = 1;

struct DisplayScale {
    float pixelDensity = DefaultScale;
    float contentScale = DefaultScale;
};

inline DisplayScale CalculateDisplayScale(float pixelDensity, float displayScale) {
    const float density = NumericUtils::PositiveOrFallback(pixelDensity, DefaultScale);
    const float pixelsPerLogicalUnit = NumericUtils::PositiveOrFallback(displayScale, density);
    // Windows 窗口坐标是实际像素，逻辑尺寸需乘系统倍率；Retina 后端已处理像素密度，不能重复叠加。
    const float contentScale = NumericUtils::PositiveOrFallback(pixelsPerLogicalUnit / density, DefaultScale);
    return {density, contentScale};
}

inline DisplayScale GetDisplayScale(SDL_Window* window) {
    return CalculateDisplayScale(SDL_GetWindowPixelDensity(window), SDL_GetWindowDisplayScale(window));
}

inline float GetContentScaleForRect(SDL_Rect bounds) {
    return NumericUtils::PositiveOrFallback(SDL_GetDisplayContentScale(SDL_GetDisplayForRect(&bounds)), DefaultScale);
}

inline bool IsDrawable(SDL_Window* window) {
    if (!window || (SDL_GetWindowFlags(window) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)))
        return false;
    int pixelWidth = 0, pixelHeight = 0;
    return SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight) && pixelWidth > 0 && pixelHeight > 0;
}

// 坐标均为 SDL 屏幕坐标；靠边时翻转弹出方向，再钳制到当前显示器工作区。
inline SDL_Rect PlaceAnchoredPopup(SDL_Rect anchor, SDL_Point size, SDL_Rect workArea, int gap) {
    size.x = std::min(size.x, workArea.w);
    size.y = std::min(size.y, workArea.h);
    int y = anchor.y + anchor.h + gap;
    if (y + size.y > workArea.y + workArea.h) y = anchor.y - gap - size.y;
    return {std::clamp(anchor.x + anchor.w - size.x, workArea.x, workArea.x + workArea.w - size.x),
            std::clamp(y, workArea.y, workArea.y + workArea.h - size.y), size.x, size.y};
}

inline SDL_Rect PlaceAdjacentPopup(SDL_Rect anchor, SDL_Point size, SDL_Rect workArea, int gap) {
    size.x = std::min(size.x, workArea.w);
    size.y = std::min(size.y, workArea.h);
    int x = anchor.x + anchor.w + gap;
    if (x + size.x > workArea.x + workArea.w) x = anchor.x - gap - size.x;
    return {std::clamp(x, workArea.x, workArea.x + workArea.w - size.x),
            std::clamp(anchor.y, workArea.y, workArea.y + workArea.h - size.y), size.x, size.y};
}

// 优先以选区中心对齐；小选区允许面板伸出选区，但始终保持在当前显示器工作区内。
inline SDL_Rect PlaceCenteredPopup(SDL_Rect region, SDL_Point size, SDL_Rect workArea) {
    size.x = std::min(size.x, workArea.w);
    size.y = std::min(size.y, workArea.h);
    return {std::clamp(region.x + (region.w - size.x) / 2, workArea.x, workArea.x + workArea.w - size.x),
            std::clamp(region.y + (region.h - size.y) / 2, workArea.y, workArea.y + workArea.h - size.y), size.x, size.y};
}

// 坐标和边缘宽度均使用 SDL 窗口坐标；调用方负责排除最大化及全屏窗口。
inline SDL_HitTestResult HitTestResizeBorder(SDL_Point point, int width, int height, float borderWidth) {
    if (point.x < 0 || point.y < 0 || point.x >= width || point.y >= height || borderWidth <= 0)
        return SDL_HITTEST_NORMAL;
    const bool left = point.x < borderWidth, right = point.x >= width - borderWidth;
    const bool top = point.y < borderWidth, bottom = point.y >= height - borderWidth;
    if (top) return left ? SDL_HITTEST_RESIZE_TOPLEFT : right ? SDL_HITTEST_RESIZE_TOPRIGHT : SDL_HITTEST_RESIZE_TOP;
    if (bottom) return left ? SDL_HITTEST_RESIZE_BOTTOMLEFT : right ? SDL_HITTEST_RESIZE_BOTTOMRIGHT : SDL_HITTEST_RESIZE_BOTTOM;
    if (left) return SDL_HITTEST_RESIZE_LEFT;
    return right ? SDL_HITTEST_RESIZE_RIGHT : SDL_HITTEST_NORMAL;
}

} // namespace shotlet::WindowUtils
