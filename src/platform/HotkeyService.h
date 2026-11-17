#pragma once
#include "i18n/Localization.h"
#include <string>

namespace shotlet {

// 全局快捷键修饰符（跨平台抽象）
enum HotkeyModifier {
    HOTKEY_MOD_SHIFT = 0x01,
    HOTKEY_MOD_CTRL  = 0x02,   // macOS: Control键, Windows: Ctrl键
    HOTKEY_MOD_ALT   = 0x04,
    HOTKEY_MOD_SUPER = 0x08,   // macOS: Cmd键, Windows: Win键
};

// 默认快捷键键码（平台特定的虚拟键码）
#if defined(SHOTLET_PLATFORM_MACOS)
constexpr int HOTKEY_DEFAULT_KEY_A = 0x00;  // kVK_ANSI_A
#else
constexpr int HOTKEY_DEFAULT_KEY_A = 0x41;  // VK_A = 'A' (65)
#endif

class HotkeyService {
public:
    enum class Action { Capture, History, CopyLatest, Count };
    // 注册全局快捷键
    // modifiers: HotkeyModifier 组合
    // key: 平台特定的虚拟键码（macOS: kVK_*, Windows: VK_*）
    // conflictMsg: 冲突时填充错误描述（用于弹窗提示）
    // 返回 true=成功, false=冲突或失败
    static bool Register(int modifiers, int key, std::string& conflictMsg,
                         Language language = Language::English, Action action = Action::Capture);

    // 注销全局快捷键
    static void Unregister(Action action = Action::Capture);

    // 录入快捷键时暂停触发，切换状态时丢弃旧事件。
    static void SetSuspended(bool suspended);

    // 检查快捷键是否被触发（每帧调用，自动清除标志）
    static bool Poll(Action action = Action::Capture);
};

} // namespace shotlet
