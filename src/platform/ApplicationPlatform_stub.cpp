#include "ApplicationPlatform.h"

#if defined(SHOTLET_PLATFORM_WINDOWS)
#error "ApplicationPlatform_stub.cpp should not be built for Windows"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#if defined(SHOTLET_PLATFORM_LINUX)
#include <X11/Xlib.h>
#endif

namespace shotlet {

void InitializeApplicationPlatform() {
#if defined(SHOTLET_PLATFORM_LINUX)
    XInitThreads();
#endif
}

void ApplyApplicationPlatformSDLHints() {
#if defined(SHOTLET_PLATFORM_LINUX)
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
#endif
}

void PrepareApplicationScreenCapture(IScreenCapture*) {
}

bool SupportsIdleOverlayReclamation() {
    return false;
}

void DelayAfterCaptureFrame() {
}

SDL_GPUDevice* CreateApplicationGpuDevice(bool) {
#if defined(SHOTLET_PLATFORM_LINUX)
    return SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, false, "vulkan");
#else
    return SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB |
        SDL_GPU_SHADERFORMAT_DXBC | SDL_GPU_SHADERFORMAT_DXIL |
        SDL_GPU_SHADERFORMAT_SPIRV,
        false, nullptr);
#endif
}

void DestroyApplicationGpuDevice(SDL_GPUDevice*& gpuDevice, bool) {
    if (!gpuDevice) {
        return;
    }
    SDL_DestroyGPUDevice(gpuDevice);
    gpuDevice = nullptr;
}

void ResetPlatformCursor() {
}

void WaitForOverlayGpuIdle(SDL_GPUDevice* gpuDevice) {
    if (gpuDevice) {
        SDL_WaitForGPUIdle(gpuDevice);
    }
}

} // namespace shotlet
