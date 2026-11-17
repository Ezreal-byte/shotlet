#pragma once
#include "Colors.h"
#include <SDL3/SDL_surface.h>
#include <SDL3/SDL_video.h>
#include <memory>
#include <string_view>

namespace shotlet::ApplicationIcon {

inline constexpr int ArtworkSizePixels = 24;
inline constexpr int WindowArtworkScale = 2;
inline constexpr int WindowSizePixels = ArtworkSizePixels * WindowArtworkScale;
using Surface = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;

namespace Detail {
inline constexpr std::string_view CoverageLevels = "0123456789abcdef";
// 每个十六进制字符表示像素覆盖率；托盘和窗口复用图形，保留圆角抗锯齿和手柄镂空。
inline constexpr std::string_view Coverage =
    "000000000000000000000000"
    "0018bccccca11acccccb8100"
    "02dffffffff22ffffffffd20"
    "0afc5544433003444445cfa0"
    "1ef300000400000000003fd0"
    "1ef200002fa0000000002fe1"
    "1ef200003ff6000000002fe1"
    "1ef200001dff300000002fe1"
    "1ef2000007ffc00000000430"
    "1ff2000000cff90000036960"
    "09a00000003fff5159cfffb0"
    "000000000006fffeffffe800"
    "067000046403effffda62660"
    "1ff201bfffdefffc20001ff1"
    "1ef20af74aff68fd52002fe1"
    "1ef21f9000ed03ffff802fe1"
    "1ef21f9001ea05fa6bf41fe1"
    "1ef20cf64bf40cd000ea1fe1"
    "1ef202dfff700dc000eb1fe1"
    "1ef30004510008f739f52fd0"
    "0afc5543343001bfffa4cfa0"
    "02dffffffff20004744ffd20"
    "0018bccccca10000003b8100"
    "000000000000000000000000";
static_assert(Coverage.size() == ArtworkSizePixels * ArtworkSizePixels);
} // namespace Detail

// 画布与图形使用实际像素；透明留白居中，不能再乘窗口的内容缩放。
inline Surface CreateSurface(ImU32 color, int canvasSizePixels = ArtworkSizePixels) {
    Surface icon(nullptr, SDL_DestroySurface);
    if (canvasSizePixels < ArtworkSizePixels || (canvasSizePixels - ArtworkSizePixels) % 2 != 0)
        return icon;
    icon.reset(SDL_CreateSurface(canvasSizePixels, canvasSizePixels, SDL_PIXELFORMAT_RGBA32));
    if (!icon || !SDL_FillSurfaceRect(icon.get(), nullptr, Colors::Transparent))
        return Surface(nullptr, SDL_DestroySurface);
    const int paddingPixels = (canvasSizePixels - ArtworkSizePixels) / 2;
    const Uint8 red = (color >> IM_COL32_R_SHIFT) & 255;
    const Uint8 green = (color >> IM_COL32_G_SHIFT) & 255;
    const Uint8 blue = (color >> IM_COL32_B_SHIFT) & 255;
    const Uint8 opacity = (color >> IM_COL32_A_SHIFT) & 255;
    for (int y = 0; y < ArtworkSizePixels; ++y) {
        for (int x = 0; x < ArtworkSizePixels; ++x) {
            const auto coverage = Detail::CoverageLevels.find(Detail::Coverage[y * ArtworkSizePixels + x]);
            if (coverage == 0) continue;
            const auto alpha = static_cast<Uint8>(coverage * opacity / (Detail::CoverageLevels.size() - 1));
            const SDL_Rect pixel{x + paddingPixels, y + paddingPixels, 1, 1};
            if (!SDL_FillSurfaceRect(icon.get(), &pixel, SDL_MapSurfaceRGBA(icon.get(), red, green, blue, alpha)))
                return Surface(nullptr, SDL_DestroySurface);
        }
    }
    return icon;
}

inline bool SetWindowIcon(SDL_Window* window) {
    if (!window) return false;
    auto artwork = CreateSurface(Colors::ApplicationAccent);
    if (!artwork) return false;
    Surface icon(SDL_ScaleSurface(artwork.get(), WindowSizePixels, WindowSizePixels, SDL_SCALEMODE_LINEAR), SDL_DestroySurface);
    // SDL 复制图标数据并管理原生图标句柄，临时表面可在返回时释放。
    return icon && SDL_SetWindowIcon(window, icon.get());
}

} // namespace shotlet::ApplicationIcon
