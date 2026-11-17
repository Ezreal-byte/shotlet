#include "Localization.h"
#include <SDL3/SDL_locale.h>
#include <SDL3/SDL_stdinc.h>
#include <memory>

namespace shotlet {

Language ResolveLanguage(LanguagePreference preference) {
    if (preference == LanguagePreference::ChineseSimplified) return Language::ChineseSimplified;
    if (preference == LanguagePreference::English) return Language::English;
    const std::unique_ptr<SDL_Locale*, decltype(&SDL_free)> locales(SDL_GetPreferredLocales(nullptr), SDL_free);
    if (locales) {
        for (auto cursor = locales.get(); *cursor; ++cursor) {
            Language language;
            if (MatchLocale((*cursor)->language, (*cursor)->country ? (*cursor)->country : "", language))
                return language;
        }
    }
    return Language::English;
}

} // namespace shotlet
