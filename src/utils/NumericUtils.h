#pragma once
#include <cmath>

namespace shotlet::NumericUtils {

// 将无效或非正的测量值替换为调用方提供的有效回退值。
inline float PositiveOrFallback(float value, float fallback) {
    return std::isfinite(value) && value > 0 ? value : fallback;
}

} // namespace shotlet::NumericUtils
