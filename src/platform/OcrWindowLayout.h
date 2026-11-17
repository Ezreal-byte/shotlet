#pragma once

namespace shotlet {

struct OcrWindowRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    int Width() const { return right - left; }
    int Height() const { return bottom - top; }
};

struct OcrWindowLayout {
    OcrWindowRect image;
    OcrWindowRect divider;
    OcrWindowRect text;
};

OcrWindowLayout CalculateOcrWindowLayout(int clientWidth,
                                         int clientHeight,
                                         int dividerWidth,
                                         float textPanelRatio);

} // namespace shotlet
