#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>

namespace shotlet::wheel {

// CGEvent 滚轮字段 ↔ ScrollPlanner 像素意图的换算（设计 10.2"坐标/单位换算在一处统一实现"）。
// Planner 约定：deltaPixels 向下为正。
//
// CGEvent 符号约定：kCGScrollWheelEventPointDeltaAxis1 正值 = 内容向上滚（向上滚动）
// —— 与"向下为正"相反。本文件是全项目唯一的符号翻转点，实测不符时只改这里。
// （符号约定为 spike 验证项，见 ScrollDemoMain 实测）

// CGEvent point delta → 像素意图。scaleFactor 为选区所在显示器 像素/点（Retina = 2.0）。
[[nodiscard]] inline int64_t PointDeltaToPixelIntent(int32_t pointDelta, double scaleFactor) {
    if (scaleFactor <= 0.0) {
        scaleFactor = 1.0;  // 无效缩放系数防御：按 1x 处理
    }
    // 符号翻转 + 点转像素
    return -static_cast<int64_t>(std::llround(static_cast<double>(pointDelta) * scaleFactor));
}

// 像素意图（向下为正）→ CGEventCreateScrollWheelEvent2 wheel1 值（同符号翻转）
[[nodiscard]] inline int32_t PixelIntentToWheelDelta(int64_t pixelDelta, double scaleFactor = 1.0) {
    // 捕获尺寸是物理像素，合成事件使用屏幕点；Retina 下先还原坐标单位。
    if (!std::isfinite(scaleFactor) || scaleFactor <= 0.0) scaleFactor = 1.0;
    const double points = std::clamp(static_cast<double>(pixelDelta) / scaleFactor,
                                     -2147483647.0, 2147483647.0);
    // 钳制后取负，保证大步长不会溢出或反向。
    return static_cast<int32_t>(-std::llround(points));
}

// line 单位 → 像素意图（离散滚轮鼠标退路：line delta 无 point delta 时）。
// 每行按 10pt 估算（macOS 默认滚动步长）；意图只是注入量参考，真实 dy 由图像匹配得出，
// 精度不敏感。
[[nodiscard]] inline int64_t LineDeltaToPixelIntent(int32_t lineDelta, double scaleFactor,
                                                    double pointsPerLine = 10.0) {
    if (scaleFactor <= 0.0) {
        scaleFactor = 1.0;
    }
    if (pointsPerLine <= 0.0) {
        pointsPerLine = 10.0;
    }
    // 符号约定与 point delta 一致（正值=向上），同样翻转
    return -static_cast<int64_t>(std::llround(
        static_cast<double>(lineDelta) * pointsPerLine * scaleFactor));
}

}  // namespace shotlet::wheel
