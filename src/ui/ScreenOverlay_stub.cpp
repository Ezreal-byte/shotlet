#include "ScreenOverlay.h"
#include <SDL3/SDL.h>

namespace shotlet {

    void ScreenOverlay::HideSDLWindow() {
        if (m_sdlWindow) SDL_HideWindow(m_sdlWindow);
    }

    void ScreenOverlay::RevealSDLWindow() {
        if (m_sdlWindow) {
            SDL_ShowWindow(m_sdlWindow);
            SDL_RaiseWindow(m_sdlWindow);
        }
    }

    bool ScreenOverlay::CreateNativeWindow() {
        // Linux stub: 不创建原生窗口
        return true;
    }

    void ScreenOverlay::DestroyNativeWindow() {
        m_nativeWindow = nullptr;
    }

    void ScreenOverlay::UpdateNativeBackground(const unsigned char* imageData,
                                                 int pixelWidth, int pixelHeight, int rowPitch) {
        // Linux stub: 不设置底图
    }

    void ScreenOverlay::ShowNativeWindow() {
        // Linux stub: 无额外操作
    }

    void ScreenOverlay::HideNativeWindow() {
        // Linux stub: 无额外操作
    }

    void ScreenOverlay::ClearNativeBackground() {
        // Linux stub: 无额外操作
    }

} // namespace shotlet
