#include "FontProvider.h"

namespace shotlet {
    std::vector<const unsigned char*> GetCachedFontData() { return {}; }
    std::vector<int> GetCachedFontDataSizes() { return {}; }

    std::vector<std::string> GetChineseFontPaths() {
        return {};
    }

    int GetChineseFontNo() {
        return 0;
    }

    float GetTextLayerFontSize() {
        return 17.0f;
    }

    float GetTopTooltipFontSize() {
        return 15.0f;
    }

    float GetTooltipFontSize() {
        return 12.0f;
    }

    float GetOcrFontSize() {
        return 12.0f;
    }
} // namespace shotlet
