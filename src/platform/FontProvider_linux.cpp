#include <fontconfig/fontconfig.h>
#include <fstream>
#include "FontProvider.h"
#include "utils/Logger.h"

namespace shotlet {
    namespace {
        struct FontCache {
            std::string path;
            int index = 0;
            std::vector<unsigned char> data;
            FontCache() {
                auto *config = FcInitLoadConfigAndFonts();
                if (!config)
                    return;
                auto *pattern = FcNameParse(reinterpret_cast<const FcChar8 *>("sans-serif:lang=zh-cn"));
                if (!pattern) {
                    FcConfigDestroy(config);
                    return;
                }
                FcConfigSubstitute(config, pattern, FcMatchPattern);
                FcDefaultSubstitute(pattern);
                FcResult result{};
                auto *fonts = FcFontSort(config, pattern, FcTrue, nullptr, &result);
                if (fonts) {
                    for (int i = 0; i < fonts->nfont; ++i) {
                        FcChar8 *file = nullptr;
                        FcCharSet *charset = nullptr;
                        int fontIndex = 0;
                        if (FcPatternGetString(fonts->fonts[i], FC_FILE, 0, &file) != FcResultMatch ||
                            FcPatternGetCharSet(fonts->fonts[i], FC_CHARSET, 0, &charset) != FcResultMatch ||
                            !FcCharSetHasChar(charset, 0x4e2d) || !FcCharSetHasChar(charset, 'A'))
                            continue;
                        const std::string candidate(reinterpret_cast<const char *>(file));
                        if (!candidate.ends_with(".ttf") && !candidate.ends_with(".ttc") &&
                            !candidate.ends_with(".otf"))
                            continue;
                        std::ifstream input(candidate, std::ios::binary | std::ios::ate);
                        const auto size = input ? input.tellg() : std::streampos(0);
                        constexpr std::streamoff MaximumFontBytes = 128 * 1024 * 1024;
                        if (size <= 0 || size > MaximumFontBytes)
                            continue;
                        std::vector<unsigned char> bytes(static_cast<size_t>(size));
                        input.seekg(0);
                        if (!input.read(reinterpret_cast<char *>(bytes.data()), bytes.size()))
                            continue;
                        FcPatternGetInteger(fonts->fonts[i], FC_INDEX, 0, &fontIndex);
                        path = candidate;
                        index = fontIndex & 0xffff;
                        data = std::move(bytes);
                        Logger::Info("[FontProvider] Loaded CJK font {} (face {}, {} bytes)", path, index, data.size());
                        break;
                    }
                    FcFontSetDestroy(fonts);
                }
                FcPatternDestroy(pattern);
                FcConfigDestroy(config);
                if (data.empty())
                    Logger::Warning("No CJK font found. Install fonts-noto-cjk or fonts-wqy-zenhei.");
            }
        };
        const FontCache &Cache() {
            static const FontCache cache;
            return cache;
        }
    } // namespace

    std::vector<std::string> GetChineseFontPaths() {
        return Cache().path.empty() ? std::vector<std::string>{} : std::vector<std::string>{Cache().path};
    }
    int GetChineseFontNo() { return Cache().index; }
    std::vector<const unsigned char *> GetCachedFontData() {
        return Cache().data.empty() ? std::vector<const unsigned char *>{}
                                    : std::vector<const unsigned char *>{Cache().data.data()};
    }
    std::vector<int> GetCachedFontDataSizes() {
        return Cache().data.empty() ? std::vector<int>{} : std::vector<int>{static_cast<int>(Cache().data.size())};
    }
    float GetTextLayerFontSize() { return 17.0f; }
    float GetTopTooltipFontSize() { return 15.0f; }
    float GetTooltipFontSize() { return 12.0f; }
    float GetOcrFontSize() { return 12.0f; }
} // namespace shotlet
