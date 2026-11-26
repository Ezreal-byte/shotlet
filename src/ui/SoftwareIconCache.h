#pragma once

#include <map>
#include <tuple>
#include "ToolbarIcons.h"

struct SDL_Renderer;
struct SDL_Texture;

namespace shotlet {

    // Small vector icons are rasterized once; SDL's integer triangle coordinates
    // otherwise break subpixel strokes into disconnected segments.
    class SoftwareIconCache {
    public:
        explicit SoftwareIconCache(SDL_Renderer *renderer) : m_renderer(renderer) {}
        ~SoftwareIconCache();
        SoftwareIconCache(const SoftwareIconCache &) = delete;
        SoftwareIconCache &operator=(const SoftwareIconCache &) = delete;

        bool Draw(ImDrawList *drawList, ImVec2 position, float size, ToolbarIcons::IconType type, ImU32 color,
                  float thickness);

    private:
        struct Image {
            SDL_Texture *texture;
            int width, height;
            float density;
        };
        using Key = std::tuple<ToolbarIcons::IconType, int, int, int, ImU32>;
        SDL_Renderer *m_renderer;
        std::map<Key, Image> m_images;
    };

} // namespace shotlet
