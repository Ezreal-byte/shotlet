#include "Preferences.h"
#include "platform/HistoryDirectoryLock.h"
#include "third_party/nlohmann/json.hpp"
#include <fstream>

namespace shotlet {
using Json = nlohmann::json;
std::filesystem::path PreferencesPath() {
    const auto legacy = HotkeyPreferencesPath();
    return legacy.empty() ? std::filesystem::path() : legacy.parent_path() / "settings.json";
}
Preferences LoadPreferences(const std::filesystem::path& path) {
    Preferences result;
    result.captureHotkey = LoadHotkeyBinding(path.parent_path() / "hotkey.conf");
    try {
        std::ifstream input(path);
        if (!input || std::filesystem::file_size(path) > 16384) return result;
        auto json = Json::parse(input);
        if (json.at("version") != 1) return result;
        auto candidate = result;
        if (!TryParseTheme(json.at("theme").get<std::string>(), candidate.theme) ||
            !TryParseLanguage(json.at("language").get<std::string>(), candidate.language)) return result;
        const auto directory = json.at("storage_directory").get<std::string>();
        candidate.storageDirectory = std::filesystem::path(std::u8string(directory.begin(), directory.end()));
        if (!candidate.storageDirectory.empty() && !candidate.storageDirectory.is_absolute()) return result;
        // 旧配置没有日志目录时，继续使用平台默认位置。
        if (json.contains("log_directory")) {
            const auto logDirectory = json.at("log_directory").get<std::string>();
            candidate.logDirectory = std::filesystem::path(std::u8string(logDirectory.begin(), logDirectory.end()));
            if (!candidate.logDirectory.empty() && !candidate.logDirectory.is_absolute()) return result;
        }
        auto read = [&](const char* name, HotkeyBinding& binding) {
            binding = {json.at(name).at("modifiers").get<int>(), json.at(name).at("key").get<int>()};
            return binding.IsValid();
        };
        if (!read("capture_hotkey", candidate.captureHotkey) || !read("history_hotkey", candidate.historyHotkey)) return result;
        // 旧配置没有新字段，保留原有两个快捷键，新操作默认为未设置。
        if (json.contains("copy_latest_hotkey") && !read("copy_latest_hotkey", candidate.copyLatestHotkey)) return result;
        if (candidate.HasValidHotkeys() && !candidate.HasDuplicateHotkeys()) result = candidate;
    } catch (...) { /* 损坏配置使用默认值，并保留旧版截图快捷键。 */ }
    return result;
}
bool SavePreferences(const std::filesystem::path& path, const Preferences& preferences) {
    if (path.empty() || !preferences.HasValidHotkeys() || preferences.HasDuplicateHotkeys() ||
        (!preferences.storageDirectory.empty() && !preferences.storageDirectory.is_absolute()) ||
        (!preferences.logDirectory.empty() && !preferences.logDirectory.is_absolute())) return false;
    auto temporary = path;
    temporary += ".tmp";
    try {
        auto binding = [](HotkeyBinding value) { return Json{{"modifiers", value.modifiers}, {"key", value.key}}; };
        const auto directory = preferences.storageDirectory.u8string();
        const auto logDirectory = preferences.logDirectory.u8string();
        Json json{{"version", 1}, {"theme", ThemeOption(preferences.theme)},
            {"language", LanguageOption(preferences.language)},
            {"storage_directory", std::string(directory.begin(), directory.end())},
            {"log_directory", std::string(logDirectory.begin(), logDirectory.end())},
            {"capture_hotkey", binding(preferences.captureHotkey)}, {"history_hotkey", binding(preferences.historyHotkey)},
            {"copy_latest_hotkey", binding(preferences.copyLatestHotkey)}};
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << json.dump(2) << '\n';
        output.close();
        std::string error;
        if (output && ReplaceHistoryFile(temporary, path, error)) return true;
    } catch (...) {}
    std::error_code ignored;
    std::filesystem::remove(temporary, ignored);
    return false;
}
} // namespace shotlet
