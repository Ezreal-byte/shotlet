#include "platform/SystemTray.h"

#include <SDL3/SDL.h>
#include <gtest/gtest.h>
#include <string>
#include <array>
#include <map>
#include <algorithm>

struct SDL_Tray {};
struct SDL_TrayMenu {};
struct SDL_TrayEntry {};

namespace {
#if defined(SHOTLET_PLATFORM_WINDOWS)
constexpr int ExpectedIconCanvasSize = 24;
constexpr SDL_Rect ExpectedIconArtworkBounds{0, 1, 24, 22};
#else
constexpr int ExpectedIconCanvasSize = 30;
constexpr SDL_Rect ExpectedIconArtworkBounds{3, 4, 24, 22};
#endif
SDL_Rect iconArtworkBounds{};
enum class Failure { None, Surface, Draw, Tray, Menu, Entry };
Failure failure;
int liveSurfaces;
int liveTrays;
int createCount;
SDL_Tray tray;
SDL_TrayMenu menu;
SDL_TrayEntry entry;
SDL_TrayEntry hotkeyEntry;
SDL_TrayEntry recentEntry;
SDL_TrayEntry latestEntry;
SDL_TrayEntry historyEntry;
std::string hotkeyLabel;
std::array<SDL_TrayEntry, 4> appearanceEntries;
size_t appearanceEntryCount;
std::map<SDL_TrayEntry*, std::pair<SDL_TrayCallback, void*>> callbacks;
std::map<SDL_TrayEntry*, bool> checked;
int iconUpdates;
SDL_TrayCallback quitCallback;
void* quitUserdata;
std::string quitLabel;
std::string tooltip;
SDL_TrayEntryFlags entryFlags;

class SystemTrayTest : public testing::Test {
protected:
    void SetUp() override {
        failure = Failure::None;
        liveSurfaces = liveTrays = createCount = 0;
        quitCallback = nullptr;
        quitUserdata = nullptr;
        quitLabel.clear();
        hotkeyLabel.clear();
        tooltip.clear();
        entryFlags = 0;
        appearanceEntryCount = 0;
        callbacks.clear();
        checked.clear();
        iconUpdates = 0;
        iconArtworkBounds = {};
    }
    void TearDown() override {
        EXPECT_EQ(liveSurfaces, 0);
        EXPECT_EQ(liveTrays, 0);
    }
    void ExpectPlatformArtworkBounds() {
        EXPECT_EQ(iconArtworkBounds.x, ExpectedIconArtworkBounds.x);
        EXPECT_EQ(iconArtworkBounds.y, ExpectedIconArtworkBounds.y);
        EXPECT_EQ(iconArtworkBounds.w, ExpectedIconArtworkBounds.w);
        EXPECT_EQ(iconArtworkBounds.h, ExpectedIconArtworkBounds.h);
    }
};
} // namespace

// 只替换操作系统入口，测试使用生产托盘实现及真实 SDL 函数签名。
extern "C" {
SDL_Surface* SDLCALL SDL_CreateSurface(int width, int height, SDL_PixelFormat format) {
    if (failure == Failure::Surface) return nullptr;
    EXPECT_EQ(width, ExpectedIconCanvasSize);
    EXPECT_EQ(height, ExpectedIconCanvasSize);
    EXPECT_EQ(format, SDL_PIXELFORMAT_RGBA32);
    ++liveSurfaces;
    return new SDL_Surface{};
}
void SDLCALL SDL_DestroySurface(SDL_Surface* surface) {
    if (surface) {
        --liveSurfaces;
        delete surface;
    }
}
Uint32 SDLCALL SDL_MapSurfaceRGBA(SDL_Surface*, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    return Uint32(r) | (Uint32(g) << 8) | (Uint32(b) << 16) | (Uint32(a) << 24);
}
bool SDLCALL SDL_FillSurfaceRect(SDL_Surface*, const SDL_Rect* rect, Uint32 color) {
    if (failure == Failure::Draw) return false;
    if (!rect) {
        iconArtworkBounds = {};
        return true;
    }
    if ((color >> 24) == 0) return true;
    EXPECT_GE(rect->x, 0);
    EXPECT_GE(rect->y, 0);
    EXPECT_LE(rect->x + rect->w, ExpectedIconCanvasSize);
    EXPECT_LE(rect->y + rect->h, ExpectedIconCanvasSize);
    // 保存可见像素的包围盒，表面释放后仍可检查实际留白，而不只断言画布大小。
    if (iconArtworkBounds.w == 0) {
        iconArtworkBounds = *rect;
        return true;
    }
    const int left = std::min(iconArtworkBounds.x, rect->x);
    const int top = std::min(iconArtworkBounds.y, rect->y);
    const int right = std::max(iconArtworkBounds.x + iconArtworkBounds.w, rect->x + rect->w);
    const int bottom = std::max(iconArtworkBounds.y + iconArtworkBounds.h, rect->y + rect->h);
    iconArtworkBounds = {left, top, right - left, bottom - top};
    return true;
}
bool SDLCALL SDL_FillSurfaceRects(SDL_Surface*, const SDL_Rect*, int, Uint32) {
    return failure != Failure::Draw;
}
SDL_Tray* SDLCALL SDL_CreateTray(SDL_Surface* icon, const char* text) {
    EXPECT_NE(icon, nullptr);
    ++createCount;
    if (failure == Failure::Tray) return nullptr;
    tooltip = text;
    ++liveTrays;
    return &tray;
}
SDL_TrayMenu* SDLCALL SDL_CreateTrayMenu(SDL_Tray*) {
    return failure == Failure::Menu ? nullptr : &menu;
}
SDL_TrayEntry* SDLCALL SDL_InsertTrayEntryAt(SDL_TrayMenu*, int, const char* label, SDL_TrayEntryFlags flags) {
    if (std::string(label) == "最近截图" || std::string(label) == "Recent screenshots") return &recentEntry;
    if (std::string(label) == "复制最近一张截图" || std::string(label) == "Copy latest screenshot") return &latestEntry;
    if (std::string(label) == "截图历史" || std::string(label) == "Screenshot history") return &historyEntry;

    if (failure == Failure::Entry) return nullptr;
    if (std::string(label) == "Settings" || std::string(label) == "设置") {
        hotkeyLabel = label;
        return &hotkeyEntry;
    }
    if (std::string(label) != "Quit Shotlet" && std::string(label) != "退出 Shotlet") {
        return &appearanceEntries[appearanceEntryCount++ % appearanceEntries.size()];
    }
    quitLabel = label;
    entryFlags = flags;
    return &entry;
}
SDL_TrayMenu* SDLCALL SDL_CreateTraySubmenu(SDL_TrayEntry*) { return &menu; }
void SDLCALL SDL_SetTrayEntryChecked(SDL_TrayEntry* item, bool value) { checked[item] = value; }
void SDLCALL SDL_SetTrayIcon(SDL_Tray*, SDL_Surface* surface) { EXPECT_NE(surface, nullptr); ++iconUpdates; }
void SDLCALL SDL_SetTrayEntryCallback(SDL_TrayEntry* item, SDL_TrayCallback callback, void* userdata) {
    callbacks[item] = {callback, userdata};
    if (item != &entry) return;
    quitCallback = callback;
    quitUserdata = userdata;
}
void SDLCALL SDL_DestroyTray(SDL_Tray*) {
    --liveTrays;
    quitCallback = nullptr;
    quitUserdata = nullptr;
}
SDL_MouseButtonFlags SDLCALL SDL_GetGlobalMouseState(float* x, float* y) {
    *x = 0; *y = 0; return 0;
}
}

TEST_F(SystemTrayTest, MenuRequestsQuitWithoutDestroyingTrayInsideCallback) {
    shotlet::SystemTray systemTray;
    ASSERT_TRUE(systemTray.Create(shotlet::Language::ChineseSimplified));
    EXPECT_EQ(tooltip, "Shotlet - 后台运行");
    EXPECT_EQ(quitLabel, "退出 Shotlet");
    EXPECT_EQ(entryFlags, SDL_TRAYENTRY_BUTTON);
    EXPECT_FALSE(systemTray.IsQuitRequested());
    ASSERT_NE(quitCallback, nullptr);
    quitCallback(quitUserdata, &entry);
    EXPECT_TRUE(systemTray.IsQuitRequested());
    EXPECT_EQ(liveTrays, 1);
    EXPECT_EQ(liveSurfaces, 0);
}

TEST_F(SystemTrayTest, RecentCaptureRequestIsDeferredConsumedOnceAndClearedOnDestroy) {
    shotlet::SystemTray tray;
    ASSERT_TRUE(tray.Create());
    auto [callback, data] = callbacks[&recentEntry];
    ASSERT_NE(callback, nullptr);
    callback(data, &recentEntry);
    EXPECT_TRUE(tray.ConsumeRecentRequest());
    EXPECT_FALSE(tray.ConsumeRecentRequest());
    EXPECT_FALSE(tray.IsQuitRequested());
    callback(data, &recentEntry);
    tray.Destroy();
    EXPECT_FALSE(tray.ConsumeRecentRequest());
}

TEST_F(SystemTrayTest, CopyLatestRequestIsDeferredAndClearedAfterConsumption) {
    shotlet::SystemTray tray; ASSERT_TRUE(tray.Create());
    auto [callback, data] = callbacks[&latestEntry]; ASSERT_NE(callback, nullptr);
    callback(data, &latestEntry);
    EXPECT_TRUE(tray.ConsumeCopyLatestRequest()); EXPECT_FALSE(tray.ConsumeCopyLatestRequest());
    EXPECT_FALSE(tray.IsQuitRequested());
}

TEST_F(SystemTrayTest, DefaultMenuUsesEnglish) {
    shotlet::SystemTray systemTray;
    ASSERT_TRUE(systemTray.Create());
    EXPECT_EQ(tooltip, "Shotlet - Running in background");
    EXPECT_EQ(quitLabel, "Quit Shotlet");
    EXPECT_EQ(hotkeyLabel, "Settings");
}

TEST_F(SystemTrayTest, PlatformPaddingIsPreservedWhenSwitchingIconTheme) {
    shotlet::SystemTray systemTray;
    ASSERT_TRUE(systemTray.Create());
    ExpectPlatformArtworkBounds();
    for (const auto theme : {shotlet::Theme::Light, shotlet::Theme::Dark}) {
        systemTray.UpdateIcon(theme);
        ExpectPlatformArtworkBounds();
    }
}

TEST_F(SystemTrayTest, SettingsRequestIsDeferredAndConsumedOnce) {
    shotlet::SystemTray systemTray;
    ASSERT_TRUE(systemTray.Create(shotlet::Language::ChineseSimplified));
    EXPECT_EQ(hotkeyLabel, "设置");
    EXPECT_FALSE(systemTray.ConsumeSettingsRequest());
    auto [callback, userdata] = callbacks[&hotkeyEntry];
    ASSERT_NE(callback, nullptr);
    callback(userdata, &hotkeyEntry);
    EXPECT_EQ(liveTrays, 1);
    EXPECT_FALSE(systemTray.IsQuitRequested());
    EXPECT_TRUE(systemTray.ConsumeSettingsRequest());
    EXPECT_FALSE(systemTray.ConsumeSettingsRequest());
    callback(userdata, &hotkeyEntry);
    systemTray.Destroy();
    EXPECT_FALSE(systemTray.ConsumeSettingsRequest());
    ASSERT_TRUE(systemTray.Create());
    EXPECT_FALSE(systemTray.ConsumeSettingsRequest());
}

TEST_F(SystemTrayTest, EnglishMenuKeepsItsLanguageForItsLifetime) {
    shotlet::SystemTray systemTray;
    ASSERT_TRUE(systemTray.Create(shotlet::Language::English));
    EXPECT_EQ(tooltip, "Shotlet - Running in background");
    EXPECT_EQ(quitLabel, "Quit Shotlet");
    // 已有托盘保持创建时语言，重新创建后使用新语言。
    ASSERT_TRUE(systemTray.Create(shotlet::Language::ChineseSimplified));
    EXPECT_EQ(quitLabel, "Quit Shotlet");
    systemTray.Destroy();
    ASSERT_TRUE(systemTray.Create(shotlet::Language::ChineseSimplified));
    EXPECT_EQ(quitLabel, "退出 Shotlet");
}

TEST_F(SystemTrayTest, CreateAndDestroyAreIdempotentAndRecreationClearsQuit) {
    shotlet::SystemTray systemTray;
    ASSERT_TRUE(systemTray.Create());
    ASSERT_TRUE(systemTray.Create());
    EXPECT_EQ(createCount, 1);
    quitCallback(quitUserdata, &entry);
    systemTray.Destroy();
    systemTray.Destroy();
    EXPECT_EQ(liveTrays, 0);
    ASSERT_TRUE(systemTray.Create());
    EXPECT_FALSE(systemTray.IsQuitRequested());
    EXPECT_EQ(createCount, 2);
}

TEST_F(SystemTrayTest, InitializationFailuresReleaseResourcesAndAllowRetry) {
    for (auto point : {Failure::Surface, Failure::Draw, Failure::Tray, Failure::Menu, Failure::Entry}) {
        shotlet::SystemTray systemTray;
        failure = point;
        EXPECT_FALSE(systemTray.Create());
        EXPECT_EQ(liveSurfaces, 0);
        EXPECT_EQ(liveTrays, 0);
        EXPECT_FALSE(systemTray.IsQuitRequested());
        failure = Failure::None;
        EXPECT_TRUE(systemTray.Create());
    }
}

TEST_F(SystemTrayTest, AppearanceAndShortcutSubmenusAreReplacedByOneSettingsEntry) {
    shotlet::SystemTray systemTray;
    ASSERT_TRUE(systemTray.Create());
    EXPECT_EQ(appearanceEntryCount, 0u);
    systemTray.UpdateIcon(shotlet::Theme::Light);
    EXPECT_EQ(iconUpdates, 1);
    systemTray.Destroy();
    systemTray.UpdateIcon(shotlet::Theme::Dark);
    EXPECT_EQ(iconUpdates, 1);
}

TEST_F(SystemTrayTest, HistoryRequestIsDeferredConsumedOnceAndDoesNotQuit) {
    shotlet::SystemTray tray; ASSERT_TRUE(tray.Create());
    EXPECT_FALSE(tray.ConsumeHistoryRequest());
    auto [callback, userdata] = callbacks[&historyEntry]; ASSERT_NE(callback, nullptr);
    callback(userdata, &historyEntry);
    EXPECT_TRUE(tray.ConsumeHistoryRequest()); EXPECT_FALSE(tray.ConsumeHistoryRequest()); EXPECT_FALSE(tray.IsQuitRequested());
    callback(userdata, &historyEntry); tray.Destroy(); EXPECT_FALSE(tray.ConsumeHistoryRequest());
}
