#pragma once

#include <CoreGraphics/CoreGraphics.h>

#include "scrollcapture/ScrollUnits.h"

namespace shotlet::wheel {

// 从真实 CGEvent 提取垂直意图，供事件 tap 和平台回归测试共用。
[[nodiscard]] inline int64_t VerticalIntent(CGEventRef event, double scaleFactor) {
    const auto pointDelta = static_cast<int32_t>(
        CGEventGetIntegerValueField(event, kCGScrollWheelEventPointDeltaAxis1));
    const auto lineDelta = static_cast<int32_t>(
        CGEventGetIntegerValueField(event, kCGScrollWheelEventDeltaAxis1));
    return pointDelta != 0 ? PointDeltaToPixelIntent(pointDelta, scaleFactor)
                          : LineDeltaToPixelIntent(lineDelta, scaleFactor);
}

}  // namespace shotlet::wheel
