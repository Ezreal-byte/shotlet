#include "platform/RecordingPlaybackWindow.h"
#include "platform/ApplicationPlatform.h"
#include "platform/LocalizedText.h"
#include "recording/GifWriter.h"
#include "utils/ScopeExit.h"
#include "utils/Win32Utils.h"
#include "native_events_win.h"
#include <commctrl.h>
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace shotlet {
namespace {
constexpr int PreviewWidthPixels = 96, PreviewHeightPixels = 64, PixelChannels = 4;
constexpr int FrameDelayMs = 100, FrameCount = 2, ColorMaximum = 255;
constexpr int PixelInsetLogical = 2, ButtonSampleInsetLogical = 8;
constexpr int VolumeMidpoint = 50, FullVolume = 100;
constexpr int ProgressRangeMaximum = 100, TrackScanInsetLogical = 4;
constexpr int AntialiasMarginPixels = 1;
}

class WindowsRecordingPlaybackTest : public testing::Test {
protected:
    void SetUp() override {
        InitializeApplicationPlatform();
        ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO)) << SDL_GetError();
        m_directory = std::filesystem::temp_directory_path() /
            ("shotlet-playback-style-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(m_directory);
        const auto path = m_directory / std::filesystem::path(u8"播放预览.gif");
        std::string error;
        GifWriter writer;
        ASSERT_TRUE(writer.Open(path, PreviewWidthPixels, PreviewHeightPixels, error)) << error;
        std::vector<uint8_t> pixels(size_t(PreviewWidthPixels) * PreviewHeightPixels * PixelChannels, ColorMaximum);
        for (int frame = 0; frame < FrameCount; ++frame) {
            for (size_t pixel = 0; pixel < pixels.size(); pixel += PixelChannels) pixels[pixel] = frame ? 0 : ColorMaximum;
            ASSERT_TRUE(writer.AddBgraFrame(pixels.data(), PreviewWidthPixels * PixelChannels, false, frame * FrameDelayMs, error)) << error;
        }
        ASSERT_TRUE(writer.Finish(FrameCount * FrameDelayMs, error)) << error;
        RecordingOptions options;
        options.format = RecordingFormat::Gif;
        options.region = {0, 0, PreviewWidthPixels, PreviewHeightPixels};
        m_preview = CreateRecordingPlaybackWindow(path, options, Language::ChineseSimplified, Theme::Dark, error);
        ASSERT_NE(m_preview, nullptr) << error;
        EnumThreadWindows(GetCurrentThreadId(), [](HWND window, LPARAM result) -> BOOL {
            constexpr int ClassNameCapacity = 64;
            wchar_t name[ClassNameCapacity]{};
            GetClassNameW(window, name, ClassNameCapacity);
            if (lstrcmpW(name, L"ShotletRecordingPlayback") != 0) return TRUE;
            *reinterpret_cast<HWND*>(result) = window; return FALSE;
        }, reinterpret_cast<LPARAM>(&m_window));
        ASSERT_NE(m_window, nullptr);
        m_play = FindWindowExW(m_window, nullptr, L"BUTTON", TrWide(Language::ChineseSimplified, TextId::RecordingPause).c_str());
        ASSERT_NE(m_play, nullptr);
        m_seek = FindWindowExW(m_window, nullptr, TRACKBAR_CLASSW, TrWide(Language::ChineseSimplified, TextId::RecordingProgress).c_str());
        m_volume = FindWindowExW(m_window, nullptr, TRACKBAR_CLASSW, TrWide(Language::ChineseSimplified, TextId::RecordingVolume).c_str());
        ASSERT_NE(m_seek, nullptr);
        ASSERT_NE(m_volume, nullptr);
        m_scale = double(GetDpiForWindow(m_window)) / USER_DEFAULT_SCREEN_DPI;
    }
    void TearDown() override {
        m_preview.reset();
        std::error_code ignored;
        if (!m_directory.empty()) std::filesystem::remove_all(m_directory, ignored);
        SDL_Quit();
    }
    int Scaled(int logical) const { return int(std::lround(logical * m_scale)); }
    COLORREF Pixel(HWND window, POINT point) const {
        RECT bounds{}; GetClientRect(window, &bounds);
        if (bounds.right <= 0 || bounds.bottom <= 0) return CLR_INVALID;
        HDC screen = GetDC(window);
        if (!screen) return CLR_INVALID;
        const ScopeExit release([&] { ReleaseDC(window, screen); });
        HDC dc = CreateCompatibleDC(screen);
        if (!dc) return CLR_INVALID;
        const ScopeExit deleteDc([&] { DeleteDC(dc); });
        HBITMAP bitmap = CreateCompatibleBitmap(screen, bounds.right, bounds.bottom);
        if (!bitmap) return CLR_INVALID;
        const ScopeExit deleteBitmap([&] { DeleteObject(bitmap); });
        const auto previous = SelectObject(dc, bitmap);
        if (!previous || previous == HGDI_ERROR) return CLR_INVALID;
        const ScopeExit restore([&] { SelectObject(dc, previous); });
        // 走原生绘制消息的离屏路径，避免其它桌面窗口遮挡控件时干扰像素检查。
        SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(dc), PRF_CLIENT | PRF_ERASEBKGND);
        return GetPixel(dc, point.x, point.y);
    }
    void Redraw() const {
        SetFocus(m_window);
        SendMessageW(m_play, WM_MOUSELEAVE, 0, 0);
        SendMessageW(m_volume, WM_MOUSELEAVE, 0, 0);
        RedrawWindow(m_window, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
    }
    HWND m_window = nullptr, m_play = nullptr, m_seek = nullptr, m_volume = nullptr;
    double m_scale = 1;
    std::unique_ptr<RecordingPlaybackWindow> m_preview;
    std::filesystem::path m_directory;
};

TEST_F(WindowsRecordingPlaybackTest, FooterButtonsAndSlidersUseTheProjectPaletteInBothThemes) {
    // GIF 隐藏音量，但对应的原生滑块与 MP4 共用绘制；显示它可在无编解码器依赖下检查样式。
    ShowWindow(m_volume, SW_SHOW);
    SendMessageW(m_volume, TBM_SETPOS, TRUE, VolumeMidpoint);
    for (auto theme : {Theme::Dark, Theme::Light}) {
        m_preview->Update(theme); Redraw();
        const auto& palette = GetThemePalette(theme);
        RECT client{}, button{}, slider{}, channel{};
        GetClientRect(m_window, &client); GetClientRect(m_play, &button); GetClientRect(m_volume, &slider);
        EXPECT_EQ(Pixel(m_window, {Scaled(PixelInsetLogical), client.bottom - Scaled(PixelInsetLogical)}), Colors::ToNativeRgb(palette.panelBackground));
        EXPECT_EQ(Pixel(m_play, {0, 0}), Colors::ToNativeRgb(palette.panelBackground));
        EXPECT_EQ(Pixel(m_play, {Scaled(ButtonSampleInsetLogical), button.bottom / 2}), Colors::ToNativeRgb(palette.confirmButton));
        SendMessageW(m_volume, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
        const LONG quarter = (channel.right - channel.left) / 4;
        EXPECT_EQ(Pixel(m_volume, {channel.left + quarter, slider.bottom / 2}), Colors::ToNativeRgb(palette.confirmButton));
        EXPECT_EQ(Pixel(m_volume, {channel.right - quarter, slider.bottom / 2}), Colors::ToNativeRgb(palette.border));
    }
}

TEST_F(WindowsRecordingPlaybackTest, ThemedControlsKeepPlaybackKeyboardVolumeAndFullScreenActions) {
    ShowWindow(m_volume, SW_SHOW); Redraw();
    SendMessageW(m_play, BM_CLICK, 0, 0);
    EXPECT_EQ(Win32Utils::WindowText(m_play), TrWide(Language::ChineseSimplified, TextId::RecordingPlay));
    SendMessageW(m_window, WM_KEYDOWN, VK_SPACE, 0);
    EXPECT_EQ(Win32Utils::WindowText(m_play), TrWide(Language::ChineseSimplified, TextId::RecordingPause));
    SendMessageW(m_volume, TBM_SETPOS, TRUE, VolumeMidpoint);
    SetFocus(m_volume);
    SendMessageW(m_volume, WM_KEYDOWN, VK_RIGHT, 0);
    EXPECT_GT(SendMessageW(m_volume, TBM_GETPOS, 0, 0), VolumeMidpoint);
    EXPECT_LE(SendMessageW(m_volume, TBM_GETPOS, 0, 0), FullVolume);
    const auto style = GetWindowLongPtrW(m_window, GWL_STYLE);
    SendMessageW(m_window, WM_KEYDOWN, VK_F11, 0);
    EXPECT_EQ(GetWindowLongPtrW(m_window, GWL_STYLE) & WS_OVERLAPPEDWINDOW, 0);
    SendMessageW(m_window, WM_KEYDOWN, VK_F11, 0);
    EXPECT_EQ(GetWindowLongPtrW(m_window, GWL_STYLE), style);
    SendMessageW(m_window, WM_CLOSE, 0, 0);
    EXPECT_FALSE(m_preview->IsOpen());
}

TEST_F(WindowsRecordingPlaybackTest, AdvancingAndReversingProgressRepaintsTheEntireTrackWithoutGaps) {
    // 不能复用 Pixel() 的全量离屏重绘，它会掩盖原生局部刷新留下的裂缝。
    // 将本测试窗口和进度条置前，直接检查连续移动后仍保留在窗口表面的像素。
    SetWindowPos(m_window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    // GIF 布局复用隐藏进度条所在行放置播放按钮；单独显示进度条时，
    // 必须先隐藏其它子控件，否则兄弟窗口绘制会污染真实表面的像素。
    for (HWND control = GetWindow(m_window, GW_CHILD); control; control = GetWindow(control, GW_HWNDNEXT)) {
        if (control != m_seek) ShowWindow(control, SW_HIDE);
    }
    ShowWindow(m_seek, SW_SHOW);
    SetWindowPos(m_seek, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SendMessageW(m_seek, TBM_SETRANGE, TRUE, MAKELPARAM(0, ProgressRangeMaximum));
    HDC dc = GetDC(m_seek);
    ASSERT_NE(dc, nullptr);
    const ScopeExit release([&] { ReleaseDC(m_seek, dc); });
    for (auto theme : {Theme::Dark, Theme::Light}) {
        SCOPED_TRACE(theme == Theme::Dark ? "深色" : "浅色");
        m_preview->Update(theme); Redraw();
        SendMessageW(m_seek, WM_MOUSELEAVE, 0, 0);
        const auto& palette = GetThemePalette(theme);
        const auto verifyPosition = [&](int position) {
            SCOPED_TRACE(position);
            SendMessageW(m_seek, TBM_SETPOS, TRUE, position);
            UpdateWindow(m_seek); GdiFlush();
            RECT bounds{}, channel{}, thumb{};
            GetClientRect(m_seek, &bounds);
            SendMessageW(m_seek, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&channel));
            SendMessageW(m_seek, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
            const int centerY = int(bounds.bottom / 2), edgeInset = Scaled(TrackScanInsetLogical);
            // 原生命中矩形不等于自绘圆形滑块；轨道采样需避开最大半径及抗锯齿边缘。
            const double centerX = std::clamp((thumb.left + thumb.right) / 2.0,
                double(channel.left), double(channel.right));
            const double radiusPixels = RecordingPlaybackLayout::SliderActiveThumbRadiusLogical * m_scale;
            const int thumbLeft = int(std::floor(centerX - radiusPixels)) - AntialiasMarginPixels;
            const int thumbRight = int(std::ceil(centerX + radiusPixels)) + AntialiasMarginPixels;
            for (int x = int(channel.left) + edgeInset; x < thumbLeft; ++x)
                ASSERT_EQ(GetPixel(dc, x, centerY), Colors::ToNativeRgb(palette.confirmButton)) << "已播放部分断裂，x=" << x;
            for (int x = thumbRight; x < int(channel.right) - edgeInset; ++x)
                ASSERT_EQ(GetPixel(dc, x, centerY), Colors::ToNativeRgb(palette.border)) << "未播放部分残留旧色，x=" << x;
        };
        // 大步移动使新旧滑块区域互不相连，同时覆盖起点、终点和反向跳转。
        constexpr int PositionStep = ProgressRangeMaximum / 4;
        for (int position = 0; position <= ProgressRangeMaximum; position += PositionStep) verifyPosition(position);
        for (int position = ProgressRangeMaximum - PositionStep; position >= 0; position -= PositionStep) verifyPosition(position);
    }
}
}
