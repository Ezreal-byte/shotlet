#pragma once
#include "HotkeyService.h"
#include <filesystem>
union SDL_Event;

namespace shotlet {

struct HotkeyBinding {
    int modifiers = HOTKEY_MOD_SUPER | HOTKEY_MOD_SHIFT;
    int key = HOTKEY_DEFAULT_KEY_A;

    static HotkeyBinding Disabled() { return {0, -1}; }
    bool IsEnabled() const { return key != -1; }
    bool IsValid() const;
    bool operator==(const HotkeyBinding&) const = default;
};

// 未保存或文件损坏时返回默认值；显式删除的快捷键单独保存为禁用状态。
HotkeyBinding LoadHotkeyBinding(const std::filesystem::path& path);
// 临时文件完整写入后原子替换，失败时保留原有配置。
bool SaveHotkeyBinding(const std::filesystem::path& path, HotkeyBinding binding);
std::filesystem::path HotkeyPreferencesPath();
// 录入与显示均使用全局监听所需的平台原生键码。
HotkeyBinding HotkeyFromEvent(const SDL_Event& event);
std::string HotkeyLabel(HotkeyBinding binding, Language language);

} // namespace shotlet
