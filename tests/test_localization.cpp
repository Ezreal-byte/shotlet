#include "i18n/Localization.h"
#include <SDL3/SDL.h>
#include <gtest/gtest.h>
#include <regex>
#include <set>
#include <vector>

using namespace shotlet;

namespace {
// 每个测试恢复原来的 SDL hint，不让语言测试污染同进程中的其他测试。
class PreferredLocales {
public:
    explicit PreferredLocales(const char* value) {
        const char* original = SDL_GetHint(SDL_HINT_PREFERRED_LOCALES);
        existed = original != nullptr;
        if (original) previous = original;
        SDL_SetHintWithPriority(SDL_HINT_PREFERRED_LOCALES, value, SDL_HINT_OVERRIDE);
    }
    ~PreferredLocales() {
        if (existed) SDL_SetHintWithPriority(SDL_HINT_PREFERRED_LOCALES, previous.c_str(), SDL_HINT_OVERRIDE);
        else SDL_ResetHint(SDL_HINT_PREFERRED_LOCALES);
    }
private:
    bool existed;
    std::string previous;
};

std::set<std::string> Parameters(const char* text) {
    const std::string value(text);
    const std::regex placeholder(R"(\{([0-9]+)(?::[^}]*)?\})");
    std::set<std::string> result;
    for (auto it = std::sregex_iterator(value.begin(), value.end(), placeholder);
         it != std::sregex_iterator(); ++it) result.insert((*it)[1]);
    return result;
}

bool Parse(std::vector<std::string> args, LanguagePreference& preference, std::string& error) {
    std::vector<char*> argv;
    for (auto& arg : args) argv.push_back(arg.data());
    return ParseLanguageArguments(static_cast<int>(argv.size()), argv.data(), preference, error);
}
} // namespace

TEST(LocalizationTest, EveryEntryHasBothLanguagesAndCompatibleParameters) {
    for (size_t i = 0; i < Translations.size(); ++i) {
        SCOPED_TRACE(i);
        const auto id = static_cast<TextId>(i);
        const auto zh = Tr(Language::ChineseSimplified, id);
        const auto en = Tr(Language::English, id);
        ASSERT_NE(zh[0], '\0');
        ASSERT_NE(en[0], '\0');
        EXPECT_EQ(Parameters(zh), Parameters(en));
        // 按真实参数类型格式化每个模板，验证格式语法和类型，不能依靠回退掩盖错误。
        for (const char* text : {zh, en}) {
            std::string detail = "Ctrl+Shift+A";
            int number = 2;
            if (id == TextId::ScrollRetryCount) {
                EXPECT_NO_THROW((void)std::vformat(text, std::make_format_args(number)));
            } else {
                EXPECT_NO_THROW((void)std::vformat(text, std::make_format_args(detail, number)));
            }
        }
    }
}

TEST(LocalizationTest, FormatParametersAndCompactLabelsAreUsable) {
    EXPECT_STREQ(Tr(Language{}, TextId::SizeSmall), "Small");
    EXPECT_EQ(FormatText(Language::English, TextId::HotkeyRegisterFailed, "Ctrl+A", 1409),
              "Unable to register hotkey Ctrl+A (error 1409).");
    EXPECT_EQ(FormatText(Language::ChineseSimplified, TextId::ScrollRetryCount, 1),
              "画面有效内容不足，正在扩大滚动范围重试（1/2）");
    EXPECT_STREQ(Tr(Language::English, TextId::SizeSmallLabel), "S");
    EXPECT_STREQ(Tr(Language::English, TextId::SizeSmall), "Small");
    EXPECT_STREQ(Tr(Language::English, TextId::StrokeThin), "Thin");
}

TEST(LocalizationTest, ParsesOptionsAndRejectsInvalidValuesWithoutPartialChanges) {
    LanguagePreference preference = LanguagePreference::Auto;
    std::string error;
    EXPECT_TRUE(Parse({"shotlet", "--language", "en", "--daemon"}, preference, error));
    EXPECT_EQ(preference, LanguagePreference::English);
    EXPECT_TRUE(Parse({"shotlet", "--language=zh-CN"}, preference, error));
    EXPECT_EQ(preference, LanguagePreference::ChineseSimplified);
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"shotlet", "--language"}, {"shotlet", "--language="},
             {"shotlet", "--language", "--daemon"},
             {"shotlet", "--language=en", "--language=fr"}, {"shotlet", "--language=EN"}}) {
        EXPECT_FALSE(Parse(args, preference, error));
        EXPECT_EQ(preference, LanguagePreference::ChineseSimplified);
        EXPECT_FALSE(error.empty());
    }
    EXPECT_TRUE(Parse({"shotlet", "--language=auto"}, preference, error));
    EXPECT_EQ(preference, LanguagePreference::Auto);
    EXPECT_TRUE(error.empty());
}

TEST(LocalizationTest, MatchesScriptsAndRegionsAcrossSDLPlatformShapes) {
    struct Case { const char* language; const char* country; bool supported; Language expected; };
    for (const auto& value : std::vector<Case>{
             {"en", "US", true, Language::English}, {"EN_gb", "", true, Language::English},
             {"zh", "CN", true, Language::ChineseSimplified},
             {"zh", "SG", true, Language::ChineseSimplified},
             {"zh", "", true, Language::ChineseSimplified},
             {"zh-Hans", "CN", true, Language::ChineseSimplified},
             {"zh", "Hans", true, Language::ChineseSimplified},
             {"zh-Hans", "HK", true, Language::ChineseSimplified},
             {"zh-Hant", "CN", false, Language::English},
             {"zh", "Hant", false, Language::English},
             {"zh", "TW", false, Language::English}, {"zh", "HK", false, Language::English},
             {"zh", "MO", false, Language::English}, {"fr", "FR", false, Language::English},
             {"", "", false, Language::English}}) {
        SCOPED_TRACE(std::string(value.language) + "/" + value.country);
        Language result = Language::English;
        EXPECT_EQ(MatchLocale(value.language, value.country, result), value.supported);
        if (value.supported) EXPECT_EQ(result, value.expected);
    }
}

TEST(LocalizationTest, ResolvesPreferredListBeforeVideoInitializationAndHonorsOverride) {
    PreferredLocales locales("zh_TW,fr_FR,en_GB,zh_CN");
    EXPECT_EQ(ResolveLanguage(LanguagePreference::Auto), Language::English);
    EXPECT_EQ(ResolveLanguage(LanguagePreference::ChineseSimplified), Language::ChineseSimplified);
    EXPECT_EQ(ResolveLanguage(LanguagePreference::English), Language::English);
}

TEST(LocalizationTest, UnsupportedListFallsBackToEnglish) {
    PreferredLocales locales("ja_JP,zh_TW");
    EXPECT_EQ(ResolveLanguage(LanguagePreference::Auto), Language::English);
}

TEST(LocalizationTest, AutoSelectsChineseFromLaterPreference) {
    PreferredLocales locales("ja_JP,zh-Hans_CN,en_US");
    EXPECT_EQ(ResolveLanguage(LanguagePreference::Auto), Language::ChineseSimplified);
}
