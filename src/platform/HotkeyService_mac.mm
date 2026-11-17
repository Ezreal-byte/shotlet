#include "HotkeyService.h"
#include "utils/Logger.h"

#import <Cocoa/Cocoa.h>
#include <Carbon/Carbon.h>  // kVK_* 虚拟键码定义
#include <CoreGraphics/CoreGraphics.h>
#include <array>

namespace shotlet {

// 全局状态
static CFMachPortRef s_eventTap = nullptr;
static CFRunLoopSourceRef s_runLoopSource = nullptr;
struct Registration {
    bool registered = false, triggered = false;
    CGEventFlags flags = 0;
    CGKeyCode key = 0;
    int modifiers = 0;
};
static std::array<Registration, size_t(HotkeyService::Action::Count)> s_registrations;
static bool s_suspended = false;

// 修饰符描述
static std::string DescribeModifiers(int modifiers) {
    std::string desc;
    if (modifiers & HOTKEY_MOD_SUPER) desc += "Cmd+";
    if (modifiers & HOTKEY_MOD_CTRL)  desc += "Ctrl+";
    if (modifiers & HOTKEY_MOD_ALT)   desc += "Alt+";
    if (modifiers & HOTKEY_MOD_SHIFT) desc += "Shift+";
    return desc;
}

// 虚拟键码 → 键名
static std::string DescribeKey(int key) {
    switch (key) {
        case kVK_ANSI_A: return "A";
        case kVK_ANSI_S: return "S";
        case kVK_ANSI_D: return "D";
        case kVK_ANSI_F: return "F";
        case kVK_ANSI_Q: return "Q";
        case kVK_ANSI_W: return "W";
        case kVK_ANSI_E: return "E";
        case kVK_ANSI_R: return "R";
        case kVK_F1:  return "F1";
        case kVK_F2:  return "F2";
        case kVK_F3:  return "F3";
        case kVK_F4:  return "F4";
        case kVK_F5:  return "F5";
        case kVK_Space: return "Space";
        default: return std::to_string(key);
    }
}

// 将跨平台修饰符转换为 CGEventFlags
static CGEventFlags ToCGEventFlags(int modifiers) {
    CGEventFlags flags = 0;
    if (modifiers & HOTKEY_MOD_SUPER) flags |= kCGEventFlagMaskCommand;
    if (modifiers & HOTKEY_MOD_CTRL)  flags |= kCGEventFlagMaskControl;
    if (modifiers & HOTKEY_MOD_ALT)   flags |= kCGEventFlagMaskAlternate;
    if (modifiers & HOTKEY_MOD_SHIFT) flags |= kCGEventFlagMaskShift;
    return flags;
}

// CGEventTap 回调
static CGEventRef HotkeyCGEventCallback(CGEventTapProxy proxy, CGEventType type,
                                         CGEventRef event, void *refcon) {
    if (type == kCGEventKeyDown) {
        CGEventFlags flags = CGEventGetFlags(event);
        CGKeyCode keyCode = static_cast<CGKeyCode>(
            CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));

        // 仅比较我们关心的修饰键
        CGEventFlags modMask = kCGEventFlagMaskCommand | kCGEventFlagMaskShift |
                                kCGEventFlagMaskControl | kCGEventFlagMaskAlternate;
        for (auto& registration : s_registrations) {
            if (registration.registered && !s_suspended && !CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) &&
                (flags & modMask) == registration.flags && keyCode == registration.key) registration.triggered = true;
        }
    } else if (type == kCGEventTapDisabledByTimeout) {
        Logger::Warning("[HotkeyService] CGEventTap disabled by timeout, re-enabling");
        CGEventTapEnable(s_eventTap, true);
    }
    return event;
}

// 检查辅助功能权限，无权限时弹窗提示
static bool CheckAccessibilityPermission(std::string& conflictMsg, Language language) {
    if (AXIsProcessTrusted()) {
        return true;
    }

    Logger::Warning("[HotkeyService] Accessibility permission not granted");
    conflictMsg = Tr(language, TextId::HotkeyPermission);
    // 自动弹出系统设置引导用户授权
    NSDictionary *options = @{(__bridge id)kAXTrustedCheckOptionPrompt: @YES};
    AXIsProcessTrustedWithOptions((__bridge CFDictionaryRef)options);
    return false;
}

bool HotkeyService::Register(int modifiers, int key, std::string& conflictMsg, Language language, Action action) {
    conflictMsg.clear();
    auto& registration = s_registrations[size_t(action)];
    if (registration.registered && registration.key == key && registration.modifiers == modifiers) return true;
    if (s_eventTap) {
        registration = {true, false, ToCGEventFlags(modifiers), CGKeyCode(key), modifiers};
        return true;
    }

    std::string keyDesc = DescribeModifiers(modifiers) + DescribeKey(key);
    Logger::Info("[HotkeyService] Registering CGEventTap: {}", keyDesc);

    // 先检查辅助功能权限
    if (!CheckAccessibilityPermission(conflictMsg, language)) {
        Logger::Error("[HotkeyService] Accessibility permission not granted, cannot register hotkey");
        return false;
    }

    // 创建 CGEventTap（ListenOnly 模式：监听但不拦截事件）
    CGEventMask eventMask = (1 << kCGEventKeyDown) | (1 << kCGEventFlagsChanged);
    CFMachPortRef eventTap = CGEventTapCreate(kCGHIDEventTap, kCGTailAppendEventTap,
                                   kCGEventTapOptionListenOnly, eventMask,
                                   HotkeyCGEventCallback, nullptr);

    if (!eventTap) {
        conflictMsg = FormatText(language, TextId::HotkeyListenerFailed, keyDesc);
        Logger::Error("[HotkeyService] CGEventTapCreate failed");
        return false;
    }

    // 将 tap 加入当前线程的 run loop
    CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, eventTap, 0);
    if (!source) {
        CFRelease(eventTap);
        conflictMsg = FormatText(language, TextId::HotkeyListenerFailed, keyDesc);
        return false;
    }
    // 新监听器创建成功才替换旧注册，失败不影响当前快捷键。
    registration = {true, false, ToCGEventFlags(modifiers), CGKeyCode(key), modifiers};
    s_eventTap = eventTap;
    s_runLoopSource = source;
    CFRunLoopAddSource(CFRunLoopGetCurrent(), s_runLoopSource, kCFRunLoopCommonModes);
    CGEventTapEnable(s_eventTap, true);

    Logger::Info("[HotkeyService] Registered: {} (CGEventTap created and enabled)", keyDesc);
    return true;
}

void HotkeyService::Unregister(Action action) {
    s_registrations[size_t(action)] = {};
    for (const auto& registration : s_registrations) if (registration.registered) return;

    if (s_eventTap) {
        CGEventTapEnable(s_eventTap, false);
        if (s_runLoopSource) {
            CFRunLoopRemoveSource(CFRunLoopGetCurrent(), s_runLoopSource, kCFRunLoopCommonModes);
            CFRelease(s_runLoopSource);
            s_runLoopSource = nullptr;
        }
        CFRelease(s_eventTap);
        s_eventTap = nullptr;
    }
    Logger::Info("[HotkeyService] Unregistered");
}

void HotkeyService::SetSuspended(bool suspended) {
    if (s_suspended != suspended) for (auto& registration : s_registrations) registration.triggered = false;
    s_suspended = suspended;
}

bool HotkeyService::Poll(Action action) {
    if (!s_eventTap) return false;
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, false);
    auto& registration = s_registrations[size_t(action)];
    const bool triggered = registration.triggered;
    registration.triggered = false;
    return triggered;
}

} // namespace shotlet
