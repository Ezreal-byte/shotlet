#include "ApplicationPlatform.h"

#if !defined(SHOTLET_PLATFORM_WINDOWS)
#error "ApplicationPlatform_win.cpp should only be built for Windows"
#endif

#include "PlatformWin32.h"
#include "WindowsDpiAwareness.h"
#include "utils/Logger.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

namespace shotlet {
namespace {
SDL_GPUDevice* s_embeddedGpuDevice = nullptr;
}

void InitializeApplicationPlatform() {
    EnsureWindowsDpiAwareness();
}

void ApplyApplicationPlatformSDLHints() {
    SDL_SetHint(SDL_HINT_WINDOWS_RAW_KEYBOARD, "1");
}

void PrepareApplicationScreenCapture(IScreenCapture*) {
}

bool SupportsIdleOverlayReclamation() {
    return false;
}

void DelayAfterCaptureFrame() {
    SDL_Delay(16);
}

SDL_GPUDevice* CreateApplicationGpuDevice(bool embeddedMode) {
    if (embeddedMode && s_embeddedGpuDevice) {
        const char* driver = SDL_GetGPUDeviceDriver(s_embeddedGpuDevice);
        Logger::Info("SDL_GPU device reused (driver: {})", driver ? driver : "unknown");
        return s_embeddedGpuDevice;
    }

    SDL_PropertiesID gpuProps = SDL_CreateProperties();
    SDL_SetBooleanProperty(gpuProps, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXBC_BOOLEAN, true);
    SDL_SetBooleanProperty(gpuProps, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_DXIL_BOOLEAN, true);
    SDL_SetBooleanProperty(gpuProps, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, false);
    SDL_SetBooleanProperty(gpuProps, SDL_PROP_GPU_DEVICE_CREATE_VERBOSE_BOOLEAN, false);
    SDL_SetStringProperty(gpuProps, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "direct3d12");
    SDL_GPUDevice* device = SDL_CreateGPUDeviceWithProperties(gpuProps);
    SDL_DestroyProperties(gpuProps);

    if (embeddedMode) {
        s_embeddedGpuDevice = device;
    }
    return device;
}

void DestroyApplicationGpuDevice(SDL_GPUDevice*& gpuDevice, bool embeddedMode) {
    if (!gpuDevice) {
        return;
    }
    if (embeddedMode && gpuDevice == s_embeddedGpuDevice) {
        gpuDevice = nullptr;
        return;
    }

    SDL_DestroyGPUDevice(gpuDevice);
    gpuDevice = nullptr;
}

void ResetPlatformCursor() {
    ::SetCursor(::LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)));
}

void WaitForOverlayGpuIdle(SDL_GPUDevice*) {
}

} // namespace shotlet
