#include "TransparentWindow.h"

#include <SDL3/SDL.h>

#if !defined(SHOTLET_PLATFORM_MACOS)
#if !defined(SHOTLET_PLATFORM_WINDOWS)

namespace shotlet {

void ConfigureWindowTransparent(SDL_Window*) {
    // The X11 capture is drawn as an opaque SDL_GPU background; no native backdrop is needed.
}

void SetWindowTransparent(SDL_Window* window) {
    // Linux 透明窗口通过 SDL_GPU 渲染实现
    // 不需要额外操作
}

void SetWindowOpaque(SDL_Window* window) {
    // 不需要额外操作
}

void ForceWindowFocus(SDL_Window* window) {
    // 依赖 SDL_RaiseWindow 作为 fallback
    if (window) {
        SDL_RaiseWindow(window);
    }
}

bool RestoreActivationPolicy() {
    // Linux/stub 无需恢复激活策略
    return true;
}

} // namespace shotlet

#endif
#endif
