#pragma once
#include "platform/FontProvider.h"
#include <imgui.h>

namespace shotlet::ImGuiUtils {
// 字体缓存归 FontProvider 所有，atlas 不接管缓存；失败时按相同字号回退。
inline ImFont* AddUiFont(ImFontAtlas& atlas, float logicalSize) {
    ImFontConfig config;
    config.FontNo = GetChineseFontNo();
    config.FontDataOwnedByAtlas = false;
    const auto data = GetCachedFontData();
    const auto sizes = GetCachedFontDataSizes();
    for (size_t i = 0; i < data.size() && i < sizes.size(); ++i) {
        if (auto* font = atlas.AddFontFromMemoryTTF(const_cast<unsigned char*>(data[i]), sizes[i], logicalSize, &config))
            return font;
    }
    config.FontDataOwnedByAtlas = true;
    for (const auto& path : GetChineseFontPaths())
        if (auto* font = atlas.AddFontFromFileTTF(path.c_str(), logicalSize, &config)) return font;
    ImFontConfig fallback;
    fallback.SizePixels = logicalSize;
    return atlas.AddFontDefault(&fallback);
}
} // namespace shotlet::ImGuiUtils
