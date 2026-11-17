#include "FontProvider.h"

#if !defined(SHOTLET_PLATFORM_WINDOWS)
#error "FontProvider_win.cpp must only be compiled on Windows"
#endif

#include "platform/PlatformWin32.h"

#include <fstream>
#include <utility>

namespace shotlet {

std::vector<std::string> GetChineseFontPaths() {
    return {
        "C:\\Windows\\Fonts\\msyh.ttc",
        "C:\\Windows\\Fonts\\simhei.ttf",
        "C:\\Windows\\Fonts\\simsun.ttc",
    };
}

int GetChineseFontNo() {
    return 0;
}

struct FontDataCache {
    std::vector<std::vector<unsigned char>> dataBuffers;
    std::vector<int> dataSizes;
    bool loaded = false;
};

static FontDataCache& GetFontDataCache() {
    static FontDataCache cache;
    if (!cache.loaded) {
        auto paths = GetChineseFontPaths();
        for (const auto& path : paths) {
            std::ifstream file(path, std::ios::binary | std::ios::ate);
            if (!file) {
                continue;
            }

            std::streamsize size = file.tellg();
            if (size <= 0) {
                continue;
            }

            std::vector<unsigned char> buffer(static_cast<size_t>(size));
            file.seekg(0, std::ios::beg);
            if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
                continue;
            }

            cache.dataSizes.push_back(static_cast<int>(size));
            cache.dataBuffers.push_back(std::move(buffer));
        }
        cache.loaded = true;
    }
    return cache;
}

std::vector<const unsigned char*> GetCachedFontData() {
    auto& cache = GetFontDataCache();
    std::vector<const unsigned char*> result;
    for (auto& buffer : cache.dataBuffers) {
        result.push_back(buffer.data());
    }
    return result;
}

std::vector<int> GetCachedFontDataSizes() {
    auto& cache = GetFontDataCache();
    return cache.dataSizes;
}

float GetTextLayerFontSize() {
    return 23.8f;
}

float GetTopTooltipFontSize() {
    return 21.0f;
}

float GetTooltipFontSize() {
    return 16.8f;
}

float GetOcrFontSize() {
    return 12.0f;
}

} // namespace shotlet
