#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <imgui.h>
#include <imgui_internal.h>
#include "utils/NumericUtils.h"
#include "utils/WindowChrome.h"

namespace shotlet {
namespace ImageEditorLayout {
    constexpr float ViewportPadding = 16.0f;
    constexpr float StatusHeight = 34.0f;
    constexpr float ResizeBorder = 5.0f;
    constexpr float MinimumWindowWidth = 760.0f;
    constexpr float MinimumWindowHeight = 360.0f;
    // 初始窗口的实际像素预算，与工具栏使用的逻辑尺寸分开，不能再乘系统 DPI 倍率。
    constexpr float TargetWindowWidthPixels = 1560.0f;
    constexpr float MinimumInitialWindowHeightPixels = 1000.0f;
    constexpr float ScreenMargin = 48.0f;
}

// 输入工作区、输出窗口尺寸均为 SDL 窗口坐标；图片尺寸为实际像素。
// pixelDensity 只换算像素预算，uiScale 只缩放控件、屏幕留白与最小可用尺寸。
inline ImVec2 ImageEditorInitialWindowSize(ImVec2 imagePixelSize, ImVec2 workAreaWindowSize,
                                          float pixelDensity, float uiScale = 1.0f) {
    pixelDensity = NumericUtils::PositiveOrFallback(pixelDensity, 1.0f);
    uiScale = NumericUtils::PositiveOrFallback(uiScale, 1.0f);
    const float screenInset = 2 * ImageEditorLayout::ScreenMargin * uiScale;
    const ImVec2 available(std::max(1.0f, workAreaWindowSize.x - screenInset),
                           std::max(1.0f, workAreaWindowSize.y - screenInset));
    // 极高 DPI 下优先容纳工具与窗口按钮；窄屏仍以工作区边界为限。
    const float minimumWidth = ImageEditorLayout::MinimumWindowWidth * uiScale;
    const float width = std::min(available.x,
        std::max(ImageEditorLayout::TargetWindowWidthPixels / pixelDensity, minimumWidth));
    const float viewportPadding = 2 * ImageEditorLayout::ViewportPadding;
    const float imageWindowWidth = std::max(1.0f, imagePixelSize.x / pixelDensity);
    const float widthZoom = std::min(1.0f, std::max(1.0f, width - viewportPadding) / imageWindowWidth);
    const float chromeHeight = (WindowChrome::TitleBarHeight + ImageEditorLayout::StatusHeight) * uiScale;
    const float desiredHeight = imagePixelSize.y / pixelDensity * widthZoom + chromeHeight + viewportPadding;
    const float height = std::min(available.y, std::max({
        ImageEditorLayout::MinimumInitialWindowHeightPixels / pixelDensity,
        ImageEditorLayout::MinimumWindowHeight * uiScale, desiredHeight}));
    return ImVec2(std::floor(width), std::floor(height));
}

// 标注保留原始图片坐标，预览和导出使用同一旋转变换，缩放/滚动仅影响预览。
struct ImageEditorTransform {
    ImVec2 origin{0, 0};
    float zoom = 1.0f;
    int quarterTurns = 0; // 逆时针旋转次数，图片及标注仍保留原始坐标
    ImVec2 imageSize{0, 0};
    ImVec2 RotatedSize() const {
        return quarterTurns % 2 ? ImVec2(imageSize.y, imageSize.x) : imageSize;
    }
    ImVec2 ToImage(ImVec2 screen) const {
        const ImVec2 p((screen.x - origin.x) / zoom, (screen.y - origin.y) / zoom);
        switch (quarterTurns) {
            case 1: return ImVec2(imageSize.x - p.y, p.x);
            case 2: return ImVec2(imageSize.x - p.x, imageSize.y - p.y);
            case 3: return ImVec2(p.y, imageSize.y - p.x);
            default: return p;
        }
    }
    ImVec2 ToScreen(ImVec2 image) const {
        switch (quarterTurns) {
            case 1: image = ImVec2(image.y, imageSize.x - image.x); break;
            case 2: image = ImVec2(imageSize.x - image.x, imageSize.y - image.y); break;
            case 3: image = ImVec2(imageSize.y - image.y, image.x); break;
        }
        return ImVec2(origin.x + image.x * zoom, origin.y + image.y * zoom);
    }
    ImRect ToImage(ImRect screen) const {
        const ImVec2 a = ToImage(screen.Min), b = ToImage(screen.Max);
        return ImRect(ImMin(a, b), ImMax(a, b));
    }
    ImRect ToScreen(ImRect image) const {
        const ImVec2 a = ToScreen(image.Min), b = ToScreen(image.Max);
        return ImRect(ImMin(a, b), ImMax(a, b));
    }
};
inline float ImageEditorFitZoom(ImVec2 image, ImVec2 available, bool entireImage) {
    if (image.x <= 0 || image.y <= 0) return 1.0f;
    const float widthZoom = std::max(1.0f, available.x - 2 * ImageEditorLayout::ViewportPadding) / image.x;
    const float heightZoom = std::max(1.0f, available.y - 2 * ImageEditorLayout::ViewportPadding) / image.y;
    // 整图模式允许放大小图；适应宽度仍不放大长图，避免细长图片过度放大。
    return entireImage ? std::max(0.01f, std::min(widthZoom, heightZoom))
                       : std::clamp(widthZoom, 0.01f, 1.0f);
}
// 像素缓冲必须与标称尺寸严格一致（w*h*4，RGBA8）：不一致时按 width*4 行距的
// 全量纹理上传会越界读堆内存（拼接/裁剪异常路径的防御）。
inline bool IsValidPixelBufferSize(int width, int height, std::size_t rgbaBytes) {
    return width > 0 && height > 0 &&
           rgbaBytes == static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4;
}
// 编辑器选区 → 屏幕全局锚点（Pin/OCR 结果窗口定位）：选区是图片坐标，
// 先经视图变换到编辑器窗口局部坐标（已含视口偏移），再叠加窗口全局位置。
// 与 overlay 全屏路径的 windowX + selection.Min.x（选区即屏幕坐标）语义对齐：
// 直接用来源 overlay 的 windowX 加图片坐标会把结果窗口弹到屏幕外。
inline ImVec2 EditorSelectionScreenAnchor(ImVec2 editorWindowOrigin,
                                          const ImageEditorTransform& transform,
                                          ImRect selection) {
    const ImVec2 local = transform.ToScreen(selection).Min;
    return ImVec2(editorWindowOrigin.x + local.x, editorWindowOrigin.y + local.y);
}
} // namespace shotlet
