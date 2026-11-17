#pragma once
#include <algorithm>
#include <vector>

namespace shotlet {
struct OcrImageSlice {
    int top = 0, height = 0;
    int acceptTop = 0, acceptBottom = 0; // 全图坐标，按文本框中心归属去重
};
inline std::vector<OcrImageSlice> PlanOcrSlices(int height, int block = 4096, int overlap = 128) {
    std::vector<OcrImageSlice> slices;
    if (height <= 0 || block <= 0 || overlap < 0 || overlap >= block) return slices;
    for (int top = 0; top < height;) {
        const int end = std::min(height, top + block);
        slices.push_back({top, end - top, top == 0 ? 0 : top + overlap / 2,
                          end == height ? height : end - overlap / 2});
        if (end == height) break;
        top = end - overlap;
    }
    return slices;
}
struct ImageDisplaySize { double width = 0, height = 0; };
inline ImageDisplaySize FitImageToScreen(double width, double height, double availableWidth, double availableHeight) {
    if (width <= 0 || height <= 0 || availableWidth <= 0 || availableHeight <= 0) return {};
    double ratio = std::min({1.0, availableWidth / width, availableHeight / height});
    return {width * ratio, height * ratio};
}
} // namespace shotlet
