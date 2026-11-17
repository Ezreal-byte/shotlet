#include "scrollcapture/FrameStability.h"

#include <algorithm>
#include <cstdlib>

namespace shotlet {

float ComputeFrameDiffRatio(const PixelView& a, const PixelView& b,
                            const FrameStabilityConfig& config) {
    if (a.width != b.width || a.height != b.height || a.width <= 0 || a.height <= 0) {
        return 1.0f;  // 尺寸漂移是调用方 bug，按最不稳定处理（fail loud 由调用方日志）
    }
    const int step = std::max(1, config.gridStep);
    const int threshold = std::max(0, config.sampleDiffThreshold);

    int64_t samples = 0;
    int64_t diffs = 0;
    // 行/列均按 step 采样：网格覆盖均匀，且总样本数 = ceil(h/step) × ceil(w/step)
    for (int y = 0; y < a.height; y += step) {
        const uint8_t* rowA = a.Row(y);
        const uint8_t* rowB = b.Row(y);
        for (int x = 0; x < a.width; x += step) {
            const int i = x * 4;
            ++samples;
            const int dr = std::abs(static_cast<int>(rowA[i]) - static_cast<int>(rowB[i]));
            const int dg = std::abs(static_cast<int>(rowA[i + 1]) - static_cast<int>(rowB[i + 1]));
            const int db = std::abs(static_cast<int>(rowA[i + 2]) - static_cast<int>(rowB[i + 2]));
            if (dr > threshold || dg > threshold || db > threshold) {
                ++diffs;
            }
        }
    }
    return (samples == 0) ? 0.0f
                          : static_cast<float>(static_cast<double>(diffs) /
                                               static_cast<double>(samples));
}

bool IsFrameStable(const PixelView& a, const PixelView& b,
                   const FrameStabilityConfig& config) {
    return ComputeFrameDiffRatio(a, b, config) <= config.stableDiffRatio;
}

}  // namespace shotlet
