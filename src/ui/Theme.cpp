#include "Theme.h"
#include <algorithm>

namespace shotlet {
const ThemePalette& GetThemePalette(Theme theme) {
    return theme == Theme::Light ? Colors::LightTheme : Colors::DarkTheme;
}
const SettingsPalette& GetSettingsPalette(Theme theme) {
    return theme == Theme::Light ? Colors::LightSettings : Colors::DarkSettings;
}
const HistoryPalette& GetHistoryPalette(Theme theme) {
    return theme == Theme::Light ? Colors::LightHistory : Colors::DarkHistory;
}

void ApplyImGuiTheme(ImGuiStyle& style, Theme theme) {
    if (theme == Theme::Light) ImGui::StyleColorsLight(&style);
    else ImGui::StyleColorsDark(&style);
    const auto& p = GetThemePalette(theme);
    auto set = [&](ImGuiCol index, ImU32 color) { style.Colors[index] = ImGui::ColorConvertU32ToFloat4(color); };
    set(ImGuiCol_Text, p.text);
    set(ImGuiCol_TextDisabled, p.secondaryText);
    set(ImGuiCol_WindowBg, p.panelBackground);
    set(ImGuiCol_ChildBg, p.editorBackground);
    set(ImGuiCol_PopupBg, p.panelBackground);
    set(ImGuiCol_Border, p.border);
    set(ImGuiCol_Separator, p.border);
    set(ImGuiCol_FrameBg, p.selectedBackground);
    set(ImGuiCol_FrameBgHovered, p.hoverBackground);
    set(ImGuiCol_FrameBgActive, p.selectedBackground);
    set(ImGuiCol_Button, p.selectedBackground);
    set(ImGuiCol_ButtonHovered, p.hoverBackground);
    set(ImGuiCol_ButtonActive, p.selectedBackground);
    set(ImGuiCol_CheckMark, p.accent);
    set(ImGuiCol_SliderGrab, p.accent);
    set(ImGuiCol_SliderGrabActive, p.confirm);
    set(ImGuiCol_Header, p.selectedBackground);
    set(ImGuiCol_HeaderHovered, p.hoverBackground);
    set(ImGuiCol_HeaderActive, p.selectedBackground);
}

bool TryParseTheme(std::string_view value, ThemePreference& preference) {
    if (value == "auto") preference = ThemePreference::Auto;
    else if (value == "light") preference = ThemePreference::Light;
    else if (value == "dark") preference = ThemePreference::Dark;
    else return false;
    return true;
}

const char* ThemeOption(ThemePreference preference) {
    switch (preference) {
        case ThemePreference::Light: return "light";
        case ThemePreference::Dark: return "dark";
        default: return "auto";
    }
}

bool ParseThemeArguments(int argc, char** argv, ThemePreference& preference, std::string& error) {
    auto parsed = preference;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg != "--theme" && !arg.starts_with("--theme=")) continue;
        std::string_view value;
        if (arg.starts_with("--theme=")) value = arg.substr(8);
        else if (i + 1 < argc && argv[i + 1][0] != '-') value = argv[++i];
        if (!TryParseTheme(value, parsed)) {
            error = "Invalid --theme value '" + std::string(value) + "'; expected auto, light or dark";
            return false;
        }
    }
    preference = parsed;
    error.clear();
    return true;
}

Theme ResolveTheme(ThemePreference preference, std::optional<Theme> systemTheme) {
    if (preference == ThemePreference::Light) return Theme::Light;
    if (preference == ThemePreference::Dark) return Theme::Dark;
    return systemTheme.value_or(Theme::Dark);
}
} // namespace shotlet
