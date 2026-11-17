#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace shotlet::winwheel {

// 标记限制在 32 位内，避免输入转发链路截断高位后将自身注入误判为外部输入。
inline constexpr uintptr_t kSyntheticScrollMarker = 0x54534352u;

// Windows wheel delta 正值表示向上；controller 约定向下为正。
[[nodiscard]] inline int64_t WheelDeltaToIntent(int delta) {
    return -static_cast<int64_t>(delta);
}

// 触控板事件也可能带系统注入标志，只能用本进程标记识别自身注入事件。
[[nodiscard]] inline bool IsInjectedEvent(uint32_t, uintptr_t extraInfo,
                                          uintptr_t marker) {
    return extraInfo == marker;
}

// 远程桌面和触控板可能把用户手势标记为系统注入，此类事件需要透传给目标窗口。
[[nodiscard]] inline bool IsPassThroughEvent(uint32_t flags, uintptr_t extraInfo,
                                             uintptr_t marker) {
    constexpr uint32_t kInjectedFlag = 0x00000001;
    return (flags & kInjectedFlag) != 0 && extraInfo != marker;
}

// Windows 支持高精度 wheel data，实际像素位移继续由图像匹配校准。
[[nodiscard]] inline int32_t PixelIntentToWheelData(int64_t pixelDelta) {
    const int64_t clamped = std::clamp<int64_t>(
        -pixelDelta, std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max());
    return static_cast<int32_t>(clamped);
}

}  // namespace shotlet::winwheel
