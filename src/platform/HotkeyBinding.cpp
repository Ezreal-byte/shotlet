#include "HotkeyBinding.h"
#include <SDL3/SDL.h>
#include <fstream>
#if defined(SHOTLET_PLATFORM_MACOS)
#include <Carbon/Carbon.h>
#elif defined(SHOTLET_PLATFORM_WINDOWS)
#include "platform/PlatformWin32.h"
#endif

namespace shotlet {

bool HotkeyBinding::IsValid() const {
    if (!IsEnabled()) return modifiers == 0;
    if (modifiers < 0 || (modifiers & ~0x0f)) return false;
#if defined(SHOTLET_PLATFORM_MACOS)
    if (key < 0 || key > 126) return false;
    // Escape、Tab、删除键和修饰键用于窗口操作，不允许录入。
    if (key == 53 || key == 48 || key == 51 || key == 117 || (key >= 54 && key <= 63)) return false;
    const bool functionKey = key == 64 || key == 79 || key == 80 || key == 90 ||
        (key >= 96 && key <= 101) || key == 103 || key == 105 || key == 106 ||
        key == 107 || key == 109 || key == 111 || key == 113 || key == 118 ||
        key == 120 || key == 122;
#else
    if (key < 8 || key > 254) return false;
    if (key == 8 || key == 9 || key == 27 || key == 46 || (key >= 16 && key <= 20) ||
        key == 91 || key == 92 || (key >= 160 && key <= 165)) return false;
    const bool functionKey = key >= 112 && key <= 135;
#endif
    return functionKey || (modifiers & (HOTKEY_MOD_CTRL | HOTKEY_MOD_ALT | HOTKEY_MOD_SUPER));
}

HotkeyBinding LoadHotkeyBinding(const std::filesystem::path& path) {
    std::ifstream input(path);
    int version = 0;
    HotkeyBinding binding;
    if (!(input >> version >> binding.modifiers >> binding.key) || version != 1 || !binding.IsValid()) return {};
    input >> std::ws;
    return input.eof() ? binding : HotkeyBinding{};
}

bool SaveHotkeyBinding(const std::filesystem::path& path, HotkeyBinding binding) {
    if (path.empty() || !binding.IsValid()) return false;
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) return false;
        output << "1 " << binding.modifiers << ' ' << binding.key << '\n';
        output.close();
        if (!output) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            return false;
        }
    }
    const auto source = temporary.u8string();
    const auto destination = path.u8string();
    if (SDL_RenamePath(reinterpret_cast<const char*>(source.c_str()),
                       reinterpret_cast<const char*>(destination.c_str()))) return true;
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
}

std::filesystem::path HotkeyPreferencesPath() {
    char* directory = SDL_GetPrefPath("Shotlet", "Shotlet");
    if (!directory) return {};
    const std::string utf8Directory(directory);
    const auto path = std::filesystem::path(std::u8string(utf8Directory.begin(), utf8Directory.end())) / "hotkey.conf";
    SDL_free(directory);
    return path;
}

HotkeyBinding HotkeyFromEvent(const SDL_Event& event) {
    int modifiers = 0;
    if (event.key.mod & SDL_KMOD_CTRL) modifiers |= HOTKEY_MOD_CTRL;
    if (event.key.mod & SDL_KMOD_ALT) modifiers |= HOTKEY_MOD_ALT;
    if (event.key.mod & SDL_KMOD_SHIFT) modifiers |= HOTKEY_MOD_SHIFT;
    if (event.key.mod & SDL_KMOD_GUI) modifiers |= HOTKEY_MOD_SUPER;
#if defined(SHOTLET_PLATFORM_MACOS)
    return {modifiers, event.key.raw};
#elif defined(SHOTLET_PLATFORM_WINDOWS)
    return {modifiers, int(MapVirtualKeyW(event.key.raw, MAPVK_VSC_TO_VK_EX))};
#else
    int key = int(event.key.key);
    if (key >= 'a' && key <= 'z') key -= 'a' - 'A';
    return {modifiers, key};
#endif
}

std::string HotkeyLabel(HotkeyBinding binding, Language language) {
    if (!binding.IsEnabled()) return Tr(language, TextId::SettingsHotkeyDisabled);
    std::string label;
#if defined(SHOTLET_PLATFORM_MACOS)
    if (binding.modifiers & HOTKEY_MOD_CTRL) label += "⌃ ";
    if (binding.modifiers & HOTKEY_MOD_ALT) label += "⌥ ";
    if (binding.modifiers & HOTKEY_MOD_SHIFT) label += "⇧ ";
    if (binding.modifiers & HOTKEY_MOD_SUPER) label += "⌘ ";
    const int functions[] = {122,120,99,118,96,97,98,100,101,109,103,111,105,107,113,106,64,79,80,90};
    for (int i = 0; i < 20; ++i) if (binding.key == functions[i]) return label + "F" + std::to_string(i + 1);
    switch (binding.key) {
        case 36: return label + "Return";
        case 76: return label + "Enter";
        case 49: return label + "Space";
        case 123: return label + "←";
        case 124: return label + "→";
        case 125: return label + "↓";
        case 126: return label + "↑";
        case 115: return label + "Home";
        case 119: return label + "End";
        case 116: return label + "Page Up";
        case 121: return label + "Page Down";
    }
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    auto data = source ? static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData)) : nullptr;
    std::string keyLabel;
    if (data) {
        UInt32 state = 0;
        UniChar characters[8]; UniCharCount count = 0;
        if (UCKeyTranslate(reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data)), binding.key,
            kUCKeyActionDisplay, 0, LMGetKbdType(), kUCKeyTranslateNoDeadKeysBit, &state, 8, &count, characters) == noErr && count) {
            auto text = CFStringCreateMutable(kCFAllocatorDefault, 0);
            CFStringAppendCharacters(text, characters, count); CFStringUppercase(text, nullptr);
            char utf8[128]{};
            if (CFStringGetCString(text, utf8, sizeof(utf8), kCFStringEncodingUTF8)) keyLabel = utf8;
            CFRelease(text);
        }
    }
    if (source) CFRelease(source);
    return label + (keyLabel.empty() ? "Key " + std::to_string(binding.key) : keyLabel);
#else
    if (binding.modifiers & HOTKEY_MOD_CTRL) label += "Ctrl + ";
    if (binding.modifiers & HOTKEY_MOD_ALT) label += "Alt + ";
    if (binding.modifiers & HOTKEY_MOD_SHIFT) label += "Shift + ";
    if (binding.modifiers & HOTKEY_MOD_SUPER) label += "Win + ";
    if (binding.key >= 112 && binding.key <= 135) return label + "F" + std::to_string(binding.key - 111);
#if defined(SHOTLET_PLATFORM_WINDOWS)
    UINT scan = MapVirtualKeyW(binding.key, MAPVK_VK_TO_VSC);
    if (binding.key >= VK_PRIOR && binding.key <= VK_DOWN) scan |= 0x100;
    wchar_t name[64]{}; char utf8[256]{};
    if (GetKeyNameTextW(LONG(scan << 16), name, 64) && WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8, 256, nullptr, nullptr))
        return label + utf8;
#endif
    return label + SDL_GetKeyName(SDL_Keycode(binding.key));
#endif
}

} // namespace shotlet
