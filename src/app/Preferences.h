#pragma once
#include "platform/HotkeyBinding.h"
#include "ui/Theme.h"
#include <array>

namespace shotlet {
struct Preferences {
    ThemePreference theme = ThemePreference::Auto;
    LanguagePreference language = LanguagePreference::Auto;
    std::filesystem::path storageDirectory;
    std::filesystem::path logDirectory;
    HotkeyBinding captureHotkey;
#if defined(SHOTLET_PLATFORM_MACOS)
    HotkeyBinding historyHotkey{HOTKEY_MOD_SUPER | HOTKEY_MOD_SHIFT, 4}; // H
#else
    HotkeyBinding historyHotkey{HOTKEY_MOD_SUPER | HOTKEY_MOD_SHIFT, 0x48};
#endif
    HotkeyBinding copyLatestHotkey = HotkeyBinding::Disabled();
    auto Hotkeys() const { return std::array{captureHotkey, historyHotkey, copyLatestHotkey}; }
    bool HasValidHotkeys() const {
        for (const auto binding : Hotkeys()) if (!binding.IsValid()) return false;
        return true;
    }
    bool HasDuplicateHotkeys() const {
        const auto bindings = Hotkeys();
        for (size_t i = 0; i < bindings.size(); ++i)
            for (size_t j = i + 1; j < bindings.size(); ++j)
                if (bindings[i].IsEnabled() && bindings[i] == bindings[j]) return true;
        return false;
    }
};
std::filesystem::path PreferencesPath();
Preferences LoadPreferences(const std::filesystem::path& path);
bool SavePreferences(const std::filesystem::path& path, const Preferences& preferences);
} // namespace shotlet
