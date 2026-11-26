#include "SoftwareIconCache.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cmath>
#include <imgui_internal.h>
#include <vector>

namespace shotlet {
    namespace {
        constexpr int Samples = 4;
        constexpr int Padding = 2;

        std::vector<unsigned char> RasterizeIcon(ToolbarIcons::IconType type, float size, float thickness,
                                                 float density, int width, int height, ImU32 color) {
            ImDrawListSharedData shared;
            shared.CurveTessellationTol = ImGui::GetStyle().CurveTessellationTol;
            shared.SetCircleTessellationMaxError(0.1f);
            ImDrawList geometry(&shared);
            geometry._ResetForNewFrame();
            geometry.Flags = ImDrawListFlags_None;
            geometry.PushClipRect({0, 0}, {float(width) / density, float(height) / density});
            ToolbarIcons::DrawIcon(&geometry, {Padding / density, Padding / density}, size, type, color, thickness);
            const int sampleWidth = width * Samples, sampleHeight = height * Samples;
            std::vector<ImU32> coverage(size_t(sampleWidth) * sampleHeight);
            for (auto &vertex: geometry.VtxBuffer) {
                vertex.pos.x *= density * Samples;
                vertex.pos.y *= density * Samples;
            }
            const auto edge = [](ImVec2 a, ImVec2 b, float x, float y) {
                return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
            };
            for (int i = 0; i + 2 < geometry.IdxBuffer.Size; i += 3) {
                auto a = geometry.VtxBuffer[geometry.IdxBuffer[i]].pos;
                auto b = geometry.VtxBuffer[geometry.IdxBuffer[i + 1]].pos;
                auto c = geometry.VtxBuffer[geometry.IdxBuffer[i + 2]].pos;
                float area = edge(a, b, c.x, c.y);
                if (area < 0) {
                    std::swap(b, c);
                    area = -area;
                }
                if (area < 0.00001f)
                    continue;
                const int left = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
                const int top = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}))));
                const int right = std::min(sampleWidth, int(std::ceil(std::max({a.x, b.x, c.x}))));
                const int bottom = std::min(sampleHeight, int(std::ceil(std::max({a.y, b.y, c.y}))));
                for (int y = top; y < bottom; ++y) {
                    for (int x = left; x < right; ++x) {
                        const auto inside = [&](ImVec2 p, ImVec2 q) {
                            const float value = edge(p, q, x + 0.5f, y + 0.5f);
                            return value > 0 || (value == 0 && (q.y < p.y || (q.y == p.y && q.x > p.x)));
                        };
                        if (inside(a, b) && inside(b, c) && inside(c, a))
                            coverage[size_t(y) * sampleWidth + x] = geometry.VtxBuffer[geometry.IdxBuffer[i]].col;
                    }
                }
            }
            std::vector<unsigned char> rgba(size_t(width) * height * 4, 255);
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    unsigned alpha = 0, rgb[3]{};
                    for (int sy = 0; sy < Samples; ++sy) {
                        for (int sx = 0; sx < Samples; ++sx) {
                            const auto sample = coverage[size_t(y * Samples + sy) * sampleWidth + x * Samples + sx];
                            const unsigned opacity = (sample >> IM_COL32_A_SHIFT) & 255;
                            alpha += opacity;
                            rgb[0] += ((sample >> IM_COL32_R_SHIFT) & 255) * opacity;
                            rgb[1] += ((sample >> IM_COL32_G_SHIFT) & 255) * opacity;
                            rgb[2] += ((sample >> IM_COL32_B_SHIFT) & 255) * opacity;
                        }
                    }
                    auto *pixel = rgba.data() + (size_t(y) * width + x) * 4;
                    for (int channel = 0; channel < 3; ++channel)
                        pixel[channel] = alpha ? (rgb[channel] + alpha / 2) / alpha : 255;
                    pixel[3] = (alpha + Samples * Samples / 2) / (Samples * Samples);
                }
            }
            return rgba;
        }
    } // namespace

    SoftwareIconCache::~SoftwareIconCache() {
        for (auto &[key, image]: m_images)
            SDL_DestroyTexture(image.texture);
    }

    bool SoftwareIconCache::Draw(ImDrawList *drawList, ImVec2 position, float size, ToolbarIcons::IconType type,
                                 ImU32 color, float thickness) {
        float density = ImGui::GetIO().DisplayFramebufferScale.x;
        if (!std::isfinite(density) || density <= 0)
            density = 1;
        if (!m_renderer || !std::isfinite(size) || !std::isfinite(thickness) || size <= 0 || size * density > 256 ||
            thickness <= 0)
            return false;
        // Quantize to 1/64 pixel for stable keys when layout uses fractional positions.
        // Mosaic keeps fixed black/white cells; only its border follows the tool color.
        const ImU32 rasterColor = type == ToolbarIcons::IconType::Mosaic ? color : IM_COL32_WHITE;
        const Key key{type, int(std::lround(size * 64)), int(std::lround(thickness * 64)),
                      int(std::lround(density * 64)), rasterColor};
        auto entry = m_images.find(key);
        if (entry == m_images.end()) {
            if (m_images.size() >= 128)
                return false;
            const int width = int(std::ceil(size * density)) + 2 * Padding;
            const int height = width;
            auto rgba = RasterizeIcon(type, size, thickness, density, width, height, rasterColor);
            auto *texture =
                    SDL_CreateTexture(m_renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height);
            if (!texture)
                return false;
            if (!SDL_UpdateTexture(texture, nullptr, rgba.data(), width * 4) ||
                !SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND) ||
                !SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR)) {
                SDL_DestroyTexture(texture);
                return false;
            }
            entry = m_images.emplace(key, Image{texture, width, height, density}).first;
        }
        const auto &image = entry->second;
        const ImVec2 start(position.x - Padding / image.density, position.y - Padding / image.density);
        drawList->AddImage(reinterpret_cast<ImTextureID>(image.texture), start,
                           {start.x + image.width / image.density, start.y + image.height / image.density}, {0, 0},
                           {1, 1}, type == ToolbarIcons::IconType::Mosaic ? IM_COL32_WHITE : color);
        return true;
    }
} // namespace shotlet
