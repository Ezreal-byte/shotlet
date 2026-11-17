#include "NativeSystemTray.h"
namespace shotlet {
struct PopupFocusToken {};
PopupFocusToken* RememberPopupFocus() { return nullptr; }
void ReleasePopupFocus(PopupFocusToken*, SDL_Window*, bool) {}
} // namespace shotlet
