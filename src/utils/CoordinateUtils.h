#pragma once
#include <array>
#include <algorithm>
#include <cmath>

namespace shotlet {

struct RectD {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

// 输入与输出使用相同坐标单位；保持纵横比并居中，剩余区域由调用方填充背景。
inline RectD AspectFitRect(RectD bounds, double contentWidth, double contentHeight) {
    if (!std::isfinite(contentWidth) || !std::isfinite(contentHeight) || contentWidth <= 0 || contentHeight <= 0 ||
        bounds.width <= 0 || bounds.height <= 0) return {};
    const double scale = std::min(bounds.width / contentWidth, bounds.height / contentHeight);
    const double width = contentWidth * scale, height = contentHeight * scale;
    return {bounds.x + (bounds.width - width) / 2, bounds.y + (bounds.height - height) / 2, width, height};
}

// 边框全部位于区域外，中心没有遮挡窗口，鼠标可继续操作被录制的应用。
inline std::array<RectD, 4> OuterBorderRects(RectD region, double thickness) {
    return {{{region.x - thickness, region.y - thickness, region.width + 2 * thickness, thickness},
             {region.x - thickness, region.y + region.height, region.width + 2 * thickness, thickness},
             {region.x - thickness, region.y, thickness, region.height},
             {region.x + region.width, region.y, thickness, region.height}}};
}

// 选区贴到屏幕边缘时把对应边框移入屏内；采集端仍须排除这些边框窗口。
inline std::array<RectD, 4> VisibleBorderRects(RectD region, RectD display, double thickness) {
    auto strips = OuterBorderRects(region, thickness);
    for (auto& strip : strips) {
        strip.width = std::min(strip.width, display.width); strip.height = std::min(strip.height, display.height);
        strip.x = std::clamp(strip.x, display.x, display.x + display.width - strip.width);
        strip.y = std::clamp(strip.y, display.y, display.y + display.height - strip.height);
    }
    return strips;
}

[[nodiscard]] inline double GlobalMaxY(const RectD* rects, int count) {
    double maxY = 0.0;
    if (!rects || count <= 0) return maxY;

    for (int i = 0; i < count; ++i) {
        const double top = rects[i].y + rects[i].height;
        if (i == 0 || top > maxY) {
            maxY = top;
        }
    }
    return maxY;
}

[[nodiscard]] inline double CGYToAppKitBottomY(double cgTopY,
                                               double windowHeight,
                                               double globalMaxY) {
    return globalMaxY - cgTopY - windowHeight;
}

[[nodiscard]] inline double AppKitYToCGY(double appKitY, double globalMaxY) {
    return globalMaxY - appKitY;
}

[[nodiscard]] inline double AppKitYToCGYInScreen(double appKitY,
                                                 double appKitScreenY,
                                                 double appKitScreenHeight,
                                                 double cgScreenY) {
    return cgScreenY + appKitScreenY + appKitScreenHeight - appKitY;
}

[[nodiscard]] inline double CGYToAppKitBottomYInScreen(double cgTopY,
                                                       double windowHeight,
                                                       double appKitScreenY,
                                                       double appKitScreenHeight,
                                                       double cgScreenY) {
    return appKitScreenY + appKitScreenHeight - (cgTopY - cgScreenY) - windowHeight;
}

} // namespace shotlet
