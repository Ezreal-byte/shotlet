#pragma once

#include <array>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace shotlet {

// 值初始化也采用英文，与接口默认参数和系统语言查询失败的回退一致。
enum class Language { English, ChineseSimplified };
enum class LanguagePreference { Auto, ChineseSimplified, English };

enum class TextId {
#define SHOTLET_TEXT(id, zh, en) id,
#include "Messages.inc"
#undef SHOTLET_TEXT
    Count
};

struct Translation { const char* chinese; const char* english; };
inline constexpr auto Translations = std::to_array<Translation>({
#define SHOTLET_TEXT(id, zh, en) {zh, en},
#include "Messages.inc"
#undef SHOTLET_TEXT
});
static_assert(Translations.size() == static_cast<size_t>(TextId::Count));

// 静态文案指针与进程同寿命，可供 SDL、异步任务和原生窗口安全使用。
constexpr const char* Tr(Language language, TextId id) {
    const auto index = static_cast<size_t>(id);
    if (index >= Translations.size()) return "";
    const auto& entry = Translations[index];
    return language == Language::ChineseSimplified && entry.chinese[0]
        ? entry.chinese : entry.english;
}

template<class... Args>
std::string FormatText(Language language, TextId id, Args&&... args) {
    try {
        return std::vformat(Tr(language, id), std::make_format_args(args...));
    } catch (const std::format_error&) {
        // 翻译模板损坏时尝试英文，仍失败则提供稳定的通用提示。
        try {
            return std::vformat(Tr(Language::English, id), std::make_format_args(args...));
        } catch (const std::format_error&) {
            return Tr(language, TextId::MessageFormatFailed);
        }
    }
}

bool TryParseLanguage(std::string_view value, LanguagePreference& preference);
const char* LanguageOption(LanguagePreference preference);
bool ParseLanguageArguments(int argc, char** argv, LanguagePreference& preference, std::string& error);
// 无匹配返回 false，供调用方继续检查系统首选列表中的下一项。
bool MatchLocale(std::string_view language, std::string_view country, Language& result);
// 主线程调用；无需初始化视频子系统，查询失败时回退英文。
Language ResolveLanguage(LanguagePreference preference);

} // namespace shotlet
