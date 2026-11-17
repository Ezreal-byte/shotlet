#pragma once
#include <imgui.h>
#include <optional>
#include <string>
#include <string_view>
#include "utils/Colors.h"

namespace shotlet {

enum class ThemePreference { Auto, Light, Dark };
enum class Theme { Light, Dark };

const ThemePalette& GetThemePalette(Theme theme);
const SettingsPalette& GetSettingsPalette(Theme theme);
const HistoryPalette& GetHistoryPalette(Theme theme);
// 只更新颜色，保留字体、圆角及已缩放的尺寸。
void ApplyImGuiTheme(ImGuiStyle& style, Theme theme);
bool TryParseTheme(std::string_view value, ThemePreference& preference);
const char* ThemeOption(ThemePreference preference);
bool ParseThemeArguments(int argc, char** argv, ThemePreference& preference, std::string& error);
Theme ResolveTheme(ThemePreference preference, std::optional<Theme> systemTheme);

} // namespace shotlet
