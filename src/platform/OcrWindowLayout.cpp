#include "platform/OcrWindowLayout.h"

#include <algorithm>
#include <cmath>

namespace shotlet {

OcrWindowLayout CalculateOcrWindowLayout(int clientWidth,
                                         int clientHeight,
                                         int dividerWidth,
                                         float textPanelRatio) {
    const int clientW = std::max(0, clientWidth);
    const int clientH = std::max(0, clientHeight);
    if (clientW == 0 || clientH == 0) {
        return {};
    }

    const int dividerW = std::max(0, dividerWidth);
    const float ratio = std::isfinite(textPanelRatio) && textPanelRatio > 0.0f
        ? textPanelRatio
        : 0.0f;
    int imageW = clientW;
    if (ratio > 0.0f && clientW > dividerW) {
        const int availableW = clientW - dividerW;
        imageW = std::max(1, static_cast<int>(std::lround(
            static_cast<float>(availableW) / (1.0f + ratio))));
    }
    imageW = std::clamp(imageW, 0, clientW);
    const int actualDividerW = imageW < clientW ? std::min(dividerW, clientW - imageW) : 0;
    const int textLeft = imageW + actualDividerW;

    return {
        OcrWindowRect{0, 0, imageW, clientH},
        OcrWindowRect{imageW, 0, imageW + actualDividerW, clientH},
        OcrWindowRect{textLeft, 0, clientW, clientH},
    };
}

} // namespace shotlet
