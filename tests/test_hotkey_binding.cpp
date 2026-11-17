#include "platform/HotkeyBinding.h"
#include <gtest/gtest.h>
#include <chrono>
#include <fstream>

using namespace shotlet;

namespace {
class HotkeyBindingTest : public testing::Test {
protected:
    std::filesystem::path directory;
    std::filesystem::path file;
    void SetUp() override {
        directory = std::filesystem::temp_directory_path() /
            ("shotlet-hotkey-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
        file = directory / std::filesystem::path(u8"快捷键.conf");
    }
    void TearDown() override { std::filesystem::remove_all(directory); }
};
}

TEST_F(HotkeyBindingTest, DefaultAndDisabledHaveDistinctValidRepresentations) {
    EXPECT_TRUE(HotkeyBinding{}.IsEnabled());
    EXPECT_TRUE(HotkeyBinding{}.IsValid());
    EXPECT_FALSE(HotkeyBinding::Disabled().IsEnabled());
    EXPECT_TRUE(HotkeyBinding::Disabled().IsValid());
    EXPECT_FALSE((HotkeyBinding{HOTKEY_MOD_CTRL, -1}.IsValid()));
    EXPECT_FALSE((HotkeyBinding{0x10, HOTKEY_DEFAULT_KEY_A}.IsValid()));
    EXPECT_FALSE((HotkeyBinding{0, HOTKEY_DEFAULT_KEY_A}.IsValid()));
    EXPECT_FALSE((HotkeyBinding{HOTKEY_MOD_SHIFT, HOTKEY_DEFAULT_KEY_A}.IsValid()));
    EXPECT_FALSE((HotkeyBinding{HOTKEY_MOD_CTRL, 1000}.IsValid()));
}

TEST_F(HotkeyBindingTest, ReservedEditingKeysAndModifierOnlyBindingsAreRejected) {
#if defined(SHOTLET_PLATFORM_MACOS)
    const int keys[] = {48, 51, 53, 54, 55, 56, 59, 63, 117};
    const int functionKeys[] = {122, 120, 99, 118, 96, 97, 98, 100, 101, 109, 103, 111, 105, 107, 113, 106, 64, 79, 80, 90};
#else
    const int keys[] = {8, 9, 16, 17, 18, 20, 27, 46, 91, 92, 160, 165};
    const int functionKeys[] = {112, 113, 114, 123, 124, 135};
#endif
    for (int key : keys) EXPECT_FALSE((HotkeyBinding{HOTKEY_MOD_CTRL, key}.IsValid())) << key;
    for (int key : functionKeys) EXPECT_TRUE((HotkeyBinding{0, key}.IsValid())) << key;
}

TEST_F(HotkeyBindingTest, SavesReplacesAndReloadsShortcutIncludingDeletion) {
    EXPECT_EQ(LoadHotkeyBinding(file), HotkeyBinding{});
    HotkeyBinding custom{HOTKEY_MOD_CTRL | HOTKEY_MOD_ALT, HOTKEY_DEFAULT_KEY_A};
    ASSERT_TRUE(SaveHotkeyBinding(file, custom));
    EXPECT_EQ(LoadHotkeyBinding(file), custom);
    ASSERT_TRUE(SaveHotkeyBinding(file, HotkeyBinding::Disabled()));
    EXPECT_EQ(LoadHotkeyBinding(file), HotkeyBinding::Disabled());
    ASSERT_TRUE(SaveHotkeyBinding(file, {}));
    EXPECT_EQ(LoadHotkeyBinding(file), HotkeyBinding{});
    // 保留原生路径编码，Windows 默认代码页不一定能表示中文文件名。
    auto temporary = file;
    temporary += ".tmp";
    EXPECT_FALSE(std::filesystem::exists(temporary));
}

TEST_F(HotkeyBindingTest, MalformedTruncatedAndUnknownVersionsFallBackToDefault) {
    for (const char* content : {"", "1", "1 2", "2 0 -1", "1 2 -1", "1 0 1000", "1 0 -1 extra", "garbage"}) {
        { std::ofstream output(file); output << content; }
        EXPECT_EQ(LoadHotkeyBinding(file), HotkeyBinding{}) << content;
    }
}

TEST_F(HotkeyBindingTest, InvalidValueAndWriteFailurePreserveExistingPreferences) {
    ASSERT_TRUE(SaveHotkeyBinding(file, HotkeyBinding::Disabled()));
    EXPECT_FALSE(SaveHotkeyBinding(file, {0, 1000}));
    EXPECT_EQ(LoadHotkeyBinding(file), HotkeyBinding::Disabled());
    // 临时路径被目录占用，模拟无法写入，不依赖用户权限或 root 身份。
    auto temporary = file;
    temporary += ".tmp";
    std::filesystem::create_directory(temporary);
    EXPECT_FALSE(SaveHotkeyBinding(file, {}));
    EXPECT_EQ(LoadHotkeyBinding(file), HotkeyBinding::Disabled());
    EXPECT_FALSE(SaveHotkeyBinding(directory / "missing" / "hotkey.conf", {}));
    EXPECT_FALSE(SaveHotkeyBinding({}, {}));
}
