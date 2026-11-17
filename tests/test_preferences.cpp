#include "app/Preferences.h"
#include "third_party/nlohmann/json.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <fstream>

using namespace shotlet;
namespace {
class PreferencesTest : public testing::Test {
protected:
    std::filesystem::path root, file;
    void SetUp() override {
        root=std::filesystem::temp_directory_path()/("shotlet-settings-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(root); file=root/"settings.json";
    }
    void TearDown() override { std::filesystem::remove_all(root); }
};
}
TEST_F(PreferencesTest, DefaultsIncludeTwoDifferentValidShortcutsAndAutomaticAppearanceAndLanguage) {
    const auto preferences=LoadPreferences(file);
    EXPECT_EQ(preferences.theme,ThemePreference::Auto); EXPECT_EQ(preferences.language,LanguagePreference::Auto);
    EXPECT_TRUE(preferences.captureHotkey.IsValid()); EXPECT_TRUE(preferences.historyHotkey.IsValid());
    EXPECT_NE(preferences.captureHotkey,preferences.historyHotkey); EXPECT_TRUE(preferences.storageDirectory.empty());
    EXPECT_TRUE(preferences.logDirectory.empty());
}
TEST_F(PreferencesTest, ImportsExistingScreenshotShortcutBeforeSettingsFileExists) {
    ASSERT_TRUE(SaveHotkeyBinding(root/"hotkey.conf",HotkeyBinding::Disabled()));
    EXPECT_EQ(LoadPreferences(file).captureHotkey,HotkeyBinding::Disabled());
}
TEST_F(PreferencesTest, RoundTripsUnicodeDirectoryThemeLanguageAndBothShortcuts) {
    Preferences preferences; preferences.theme=ThemePreference::Light; preferences.language=LanguagePreference::ChineseSimplified;
    preferences.storageDirectory=root/std::filesystem::path(u8"截图 历史"); preferences.historyHotkey=HotkeyBinding::Disabled();
    preferences.logDirectory = root / std::filesystem::path(u8"运行 日志");
    ASSERT_TRUE(SavePreferences(file,preferences));
    const auto restored=LoadPreferences(file);
    EXPECT_EQ(restored.theme,preferences.theme); EXPECT_EQ(restored.language,preferences.language);
    EXPECT_EQ(restored.storageDirectory,preferences.storageDirectory); EXPECT_EQ(restored.historyHotkey,preferences.historyHotkey);
    EXPECT_EQ(restored.logDirectory, preferences.logDirectory);
    EXPECT_EQ(restored.captureHotkey,preferences.captureHotkey);
}
TEST_F(PreferencesTest, InvalidAndDuplicateShortcutsAndFailedWritesKeepCommittedSettings) {
    Preferences preferences; ASSERT_TRUE(SavePreferences(file,preferences));
    preferences.historyHotkey=preferences.captureHotkey; EXPECT_FALSE(SavePreferences(file,preferences));
    EXPECT_NE(LoadPreferences(file).historyHotkey,preferences.captureHotkey);
    preferences.historyHotkey=HotkeyBinding::Disabled(); std::filesystem::create_directory(file.string()+".tmp");
    EXPECT_FALSE(SavePreferences(file,preferences)); EXPECT_TRUE(LoadPreferences(file).historyHotkey.IsEnabled());
    EXPECT_FALSE(SavePreferences({},preferences));
}
TEST_F(PreferencesTest, DamagedAndUnsupportedFilesFallBackToLegacyShortcut) {
    ASSERT_TRUE(SaveHotkeyBinding(root/"hotkey.conf",HotkeyBinding::Disabled()));
    for (const char* contents:{"broken", "{\"version\":2}", "{}", "{\"version\":1,\"theme\":\"invalid\"}"}) {
        { std::ofstream output(file); output<<contents; }
        EXPECT_EQ(LoadPreferences(file).captureHotkey,HotkeyBinding::Disabled());
    }
}

TEST_F(PreferencesTest, CopyLatestShortcutRoundTripsAndRejectsConflictsWithBothOtherActions) {
    Preferences preferences;
    EXPECT_FALSE(preferences.copyLatestHotkey.IsEnabled());
    preferences.copyLatestHotkey = {HOTKEY_MOD_CTRL | HOTKEY_MOD_ALT, preferences.captureHotkey.key};
    ASSERT_TRUE(SavePreferences(file, preferences));
    EXPECT_EQ(LoadPreferences(file).copyLatestHotkey, preferences.copyLatestHotkey);
    for (const auto conflict : {preferences.captureHotkey, preferences.historyHotkey}) {
        auto changed = preferences; changed.copyLatestHotkey = conflict;
        EXPECT_FALSE(SavePreferences(file, changed));
        EXPECT_EQ(LoadPreferences(file).copyLatestHotkey, preferences.copyLatestHotkey);
    }
    preferences.copyLatestHotkey = HotkeyBinding::Disabled();
    ASSERT_TRUE(SavePreferences(file, preferences));
    EXPECT_FALSE(LoadPreferences(file).copyLatestHotkey.IsEnabled());
}

TEST_F(PreferencesTest, ExistingSettingsWithoutCopyLatestFieldKeepTheirSavedShortcuts) {
    Preferences preferences;
    preferences.captureHotkey = HotkeyBinding::Disabled();
    ASSERT_TRUE(SavePreferences(file, preferences));
    std::ifstream input(file);
    std::string contents((std::istreambuf_iterator<char>(input)), {});
    input.close();
    const auto start = contents.find("  \"copy_latest_hotkey\":");
    ASSERT_NE(start, std::string::npos);
    const auto end = contents.find("},", start);
    ASSERT_NE(end, std::string::npos);
    contents.erase(start, end + 3 - start);
    { std::ofstream output(file); output << contents; }
    const auto restored = LoadPreferences(file);
    EXPECT_FALSE(restored.captureHotkey.IsEnabled());
    EXPECT_EQ(restored.historyHotkey, preferences.historyHotkey);
    EXPECT_FALSE(restored.copyLatestHotkey.IsEnabled());
}

TEST_F(PreferencesTest, ExistingSettingsWithoutLogDirectoryKeepTheirSavedValues) {
    Preferences preferences;
    preferences.captureHotkey = HotkeyBinding::Disabled();
    preferences.theme = ThemePreference::Light;
    ASSERT_TRUE(SavePreferences(file, preferences));
    nlohmann::json json;
    { std::ifstream input(file); input >> json; }
    json.erase("log_directory");
    { std::ofstream output(file); output << json; }
    const auto restored = LoadPreferences(file);
    EXPECT_TRUE(restored.logDirectory.empty());
    EXPECT_EQ(restored.theme, preferences.theme);
    EXPECT_EQ(restored.captureHotkey, preferences.captureHotkey);
}

TEST_F(PreferencesTest, RelativeLogDirectoryCannotReplaceSavedPreferences) {
    Preferences preferences;
    preferences.logDirectory = root / "logs";
    ASSERT_TRUE(SavePreferences(file, preferences));
    auto changed = preferences;
    changed.logDirectory = "relative/logs";
    EXPECT_FALSE(SavePreferences(file, changed));
    EXPECT_EQ(LoadPreferences(file).logDirectory, preferences.logDirectory);
}
