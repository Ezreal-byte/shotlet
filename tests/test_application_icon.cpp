#include "utils/ApplicationIcon.h"
#include <SDL3/SDL.h>
#include <gtest/gtest.h>
#include <array>
#if defined(SHOTLET_PLATFORM_WINDOWS)
#include "platform/PlatformWin32.h"
#endif

namespace shotlet {
namespace {
using Pixel = std::array<Uint8, 4>;
constexpr Uint8 TransparentAlpha = 0;
constexpr Uint8 OpaqueAlpha = 255;
constexpr int TrayPaddingPixels = 3;

Pixel ReadPixel(SDL_Surface* surface, int x, int y) {
    Pixel pixel{};
    EXPECT_TRUE(SDL_ReadSurfacePixel(surface, x, y, &pixel[0], &pixel[1], &pixel[2], &pixel[3]));
    return pixel;
}
}

TEST(ApplicationIconTest, ArtworkPreservesTransparencyAntialiasingAndApplicationColor) {
    auto icon = ApplicationIcon::CreateSurface(Colors::ApplicationAccent);
    ASSERT_NE(icon, nullptr) << SDL_GetError();
    EXPECT_EQ(icon->w, ApplicationIcon::ArtworkSizePixels);
    EXPECT_EQ(icon->h, ApplicationIcon::ArtworkSizePixels);
    const Pixel color{
        (Colors::ApplicationAccent >> IM_COL32_R_SHIFT) & OpaqueAlpha,
        (Colors::ApplicationAccent >> IM_COL32_G_SHIFT) & OpaqueAlpha,
        (Colors::ApplicationAccent >> IM_COL32_B_SHIFT) & OpaqueAlpha,
        OpaqueAlpha};
    bool hasTransparentPixel = false, hasOpaquePixel = false, hasAntialiasedPixel = false;
    for (int y = 0; y < icon->h; ++y) {
        for (int x = 0; x < icon->w; ++x) {
            const auto pixel = ReadPixel(icon.get(), x, y);
            if (pixel[3] == TransparentAlpha) {
                hasTransparentPixel = true;
                continue;
            }
            for (size_t channel = 0; channel < color.size() - 1; ++channel)
                EXPECT_EQ(pixel[channel], color[channel]);
            hasOpaquePixel |= pixel[3] == OpaqueAlpha;
            hasAntialiasedPixel |= pixel[3] < OpaqueAlpha;
        }
    }
    EXPECT_TRUE(hasTransparentPixel);
    EXPECT_TRUE(hasOpaquePixel);
    EXPECT_TRUE(hasAntialiasedPixel);
}

TEST(ApplicationIconTest, PaddedTraySurfaceCentersTheSameArtworkWithoutChangingPixels) {
    auto artwork = ApplicationIcon::CreateSurface(Colors::DarkTrayIcon);
    auto padded = ApplicationIcon::CreateSurface(Colors::DarkTrayIcon,
        ApplicationIcon::ArtworkSizePixels + TrayPaddingPixels * 2);
    ASSERT_NE(artwork, nullptr);
    ASSERT_NE(padded, nullptr);
    for (int y = 0; y < padded->h; ++y) {
        for (int x = 0; x < padded->w; ++x) {
            const auto pixel = ReadPixel(padded.get(), x, y);
            const bool padding = x < TrayPaddingPixels || y < TrayPaddingPixels ||
                x >= padded->w - TrayPaddingPixels || y >= padded->h - TrayPaddingPixels;
            if (padding) EXPECT_EQ(pixel[3], TransparentAlpha);
            else EXPECT_EQ(pixel, ReadPixel(artwork.get(), x - TrayPaddingPixels, y - TrayPaddingPixels));
        }
    }
}

#if defined(SHOTLET_PLATFORM_WINDOWS)
class ApplicationWindowIconTest : public testing::Test {
protected:
    void SetUp() override { ASSERT_TRUE(SDL_InitSubSystem(SDL_INIT_VIDEO)) << SDL_GetError(); }
    void TearDown() override { SDL_QuitSubSystem(SDL_INIT_VIDEO); }
};

TEST_F(ApplicationWindowIconTest, NativeWindowKeepsBothIconSizesAfterTemporarySurfacesAreReleased) {
    constexpr int WindowExtentPixels = 64;
    const std::unique_ptr<SDL_Window, decltype(&SDL_DestroyWindow)> window(
        SDL_CreateWindow("Shotlet icon test", WindowExtentPixels, WindowExtentPixels, SDL_WINDOW_HIDDEN), SDL_DestroyWindow);
    ASSERT_NE(window, nullptr) << SDL_GetError();
    ASSERT_TRUE(ApplicationIcon::SetWindowIcon(window.get())) << SDL_GetError();
    auto native = static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(window.get()),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    ASSERT_NE(native, nullptr);
    for (const auto kind : {ICON_SMALL, ICON_BIG}) {
        const auto icon = reinterpret_cast<HICON>(SendMessageW(native, WM_GETICON, kind, 0));
        ASSERT_NE(icon, nullptr);
        ICONINFO info{};
        ASSERT_TRUE(GetIconInfo(icon, &info));
        BITMAP bitmap{};
        const auto copiedBytes = GetObjectW(info.hbmColor, static_cast<int>(sizeof(bitmap)), &bitmap);
        // GetIconInfo 返回的位图归调用方所有，检查结果前先释放，避免断言中断造成泄漏。
        DeleteObject(info.hbmColor);
        DeleteObject(info.hbmMask);
        ASSERT_EQ(copiedBytes, static_cast<int>(sizeof(bitmap)));
        EXPECT_EQ(bitmap.bmWidth, ApplicationIcon::WindowSizePixels);
        EXPECT_EQ(bitmap.bmHeight, ApplicationIcon::WindowSizePixels);
    }
}
#endif
} // namespace shotlet
