#pragma once

#include <SDL3/SDL_gpu.h>
#include <imgui_impl_sdlgpu3.h>
#if defined(SHOTLET_PLATFORM_LINUX)
#include <imgui_impl_sdlrenderer3.h>
#endif

namespace shotlet {

    // A null GPU device selects the Linux window's software SDL_Renderer backend.
    inline void NewImGuiRendererFrame(SDL_GPUDevice *device) {
#if defined(SHOTLET_PLATFORM_LINUX)
        if (!device) {
            ImGui_ImplSDLRenderer3_NewFrame();
            return;
        }
#endif
        ImGui_ImplSDLGPU3_NewFrame();
    }

} // namespace shotlet
