#include "Appearance.h"
#include <SDL3/SDL.h>
namespace shotlet {
std::optional<Theme> QuerySystemTheme() {
    switch (SDL_GetSystemTheme()) {
        case SDL_SYSTEM_THEME_LIGHT: return Theme::Light;
        case SDL_SYSTEM_THEME_DARK: return Theme::Dark;
        default: return {};
    }
}
std::optional<Theme> QuerySystemTrayTheme() { return QuerySystemTheme(); }
void ApplyWindowTheme(SDL_Window*, Theme) {}
void ApplyPopupWindowCorners(SDL_Window*, float) {}
} // namespace shotlet
