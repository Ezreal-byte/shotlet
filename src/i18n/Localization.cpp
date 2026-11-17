#include "Localization.h"
#include <algorithm>
#include <cctype>
#include <vector>

namespace shotlet {

bool TryParseLanguage(std::string_view value, LanguagePreference& preference) {
    if (value == "auto") preference = LanguagePreference::Auto;
    else if (value == "zh-CN") preference = LanguagePreference::ChineseSimplified;
    else if (value == "en") preference = LanguagePreference::English;
    else return false;
    return true;
}

const char* LanguageOption(LanguagePreference preference) {
    switch (preference) {
        case LanguagePreference::ChineseSimplified: return "zh-CN";
        case LanguagePreference::English: return "en";
        default: return "auto";
    }
}

bool ParseLanguageArguments(int argc, char** argv, LanguagePreference& preference, std::string& error) {
    error.clear();
    auto selected = preference;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg != "--language" && !arg.starts_with("--language=")) continue;
        std::string_view value;
        if (arg == "--language") {
            if (i + 1 < argc && argv[i + 1][0] != '-') value = argv[++i];
        } else value = arg.substr(std::string_view("--language=").size());
        if (!TryParseLanguage(value, selected)) {
            error = "--language must be auto, zh-CN or en";
            return false;
        }
    }
    preference = selected;
    return true;
}

bool MatchLocale(std::string_view language, std::string_view country, Language& result) {
    std::string tag(language);
    if (!country.empty()) { tag += '-'; tag += country; }
    std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char ch) {
        return ch == '_' ? '-' : static_cast<char>(std::tolower(ch));
    });
    std::vector<std::string_view> parts;
    for (size_t begin = 0; begin < tag.size();) {
        auto end = tag.find('-', begin);
        if (end == std::string::npos) end = tag.size();
        parts.emplace_back(tag.data() + begin, end - begin);
        begin = end + 1;
    }
    if (parts.empty()) return false;
    if (parts[0] == "en") { result = Language::English; return true; }
    if (parts[0] != "zh") return false;
    const auto has = [&parts](std::string_view value) {
        return std::find(parts.begin() + 1, parts.end(), value) != parts.end();
    };
    if (has("hant")) return false;
    if (has("hans")) { result = Language::ChineseSimplified; return true; }
    if (has("tw") || has("hk") || has("mo")) return false;
    if (parts.size() == 1 || has("cn") || has("sg")) {
        result = Language::ChineseSimplified;
        return true;
    }
    return false;
}

} // namespace shotlet
