#include "ApplicationPlatform.h"

#if !defined(SHOTLET_PLATFORM_MACOS)
#error "ApplicationPlatform_mac.cpp should only be built for macOS"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <chrono>
#include "capture/backends/CaptureMacOS_SC.h"
#include "utils/Logger.h"

namespace shotlet {
    namespace {
        constexpr int PermissionCheckLimit = 30;
        constexpr auto PermissionCheckInterval = std::chrono::seconds(1);

        void RequestScreenRecordingPermission(CaptureMacOS &capture, ScreenCapturePermission permission) {
            if (permission == ScreenCapturePermission::NotDetermined) {
                Logger::Info("Screen Recording permission: Not Determined - Opening settings...");
                capture.RequestPermission();
                Logger::Warning("Please grant Screen Recording permission in the System Settings");
            } else if (permission == ScreenCapturePermission::Denied) {
                Logger::Error("Screen Recording permission: Denied");
                Logger::Warning("Please grant Screen Recording permission in:");
                Logger::Warning("System Settings > Privacy & Security > Screen Recording");
                capture.RequestPermission();
            }
        }
    } // namespace

    void InitializeApplicationPlatform() {}

    void ApplyApplicationPlatformSDLHints() {
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
        // 原生托盘不计入 SDL 托盘数量；关闭最后一个设置或历史窗口时仍须保留常驻进程。
        SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
    }

    void PrepareApplicationScreenCapture(IScreenCapture *capture) {
        auto *macCapture = dynamic_cast<CaptureMacOS *>(capture);
        if (!macCapture)
            return;

        auto permission = macCapture->CheckPermission();
        for (int retry = 0; permission != ScreenCapturePermission::Granted && retry < PermissionCheckLimit; ++retry) {
            if (retry == 0)
                RequestScreenRecordingPermission(*macCapture, permission);
            Logger::Debug("Waiting for Screen Recording permission... ({}/{})", retry + 1, PermissionCheckLimit);
            SDL_Delay(static_cast<Uint32>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(PermissionCheckInterval).count()));
            permission = macCapture->CheckPermission();
        }

        if (permission != ScreenCapturePermission::Granted) {
            Logger::Error("Screen Recording permission not granted after {} seconds",
                          PermissionCheckLimit * PermissionCheckInterval.count());
            return;
        }
        Logger::Info("Screen Recording permission: Granted!");
        // 已授权但走 ScreenCaptureKit 的机器仍需预热元数据，避免首次截图阻塞。
        if (!macCapture->IsCoreGraphicsCaptureAvailable())
            macCapture->WarmupShareableContentAsync();
    }

    bool SupportsIdleOverlayReclamation() { return true; }

    void DelayAfterCaptureFrame() {}

    SDL_GPUDevice *CreateApplicationGpuDevice(bool) {
        SDL_PropertiesID gpuProps = SDL_CreateProperties();
        SDL_SetBooleanProperty(gpuProps, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_MSL_BOOLEAN, true);
        SDL_SetBooleanProperty(gpuProps, SDL_PROP_GPU_DEVICE_CREATE_SHADERS_METALLIB_BOOLEAN, true);
        SDL_SetBooleanProperty(gpuProps, SDL_PROP_GPU_DEVICE_CREATE_DEBUGMODE_BOOLEAN, false);
        SDL_SetBooleanProperty(gpuProps, SDL_PROP_GPU_DEVICE_CREATE_VERBOSE_BOOLEAN, false);
        SDL_SetBooleanProperty(gpuProps, SDL_PROP_GPU_DEVICE_CREATE_METAL_ALLOW_MACFAMILY1_BOOLEAN, true);
        SDL_SetStringProperty(gpuProps, SDL_PROP_GPU_DEVICE_CREATE_NAME_STRING, "metal");
        SDL_GPUDevice *device = SDL_CreateGPUDeviceWithProperties(gpuProps);
        SDL_DestroyProperties(gpuProps);
        return device;
    }

    void DestroyApplicationGpuDevice(SDL_GPUDevice *&gpuDevice, bool) {
        if (!gpuDevice) {
            return;
        }
        SDL_DestroyGPUDevice(gpuDevice);
        gpuDevice = nullptr;
    }

    void ResetPlatformCursor() {}

    void WaitForOverlayGpuIdle(SDL_GPUDevice *gpuDevice) {
        if (gpuDevice) {
            SDL_WaitForGPUIdle(gpuDevice);
        }
    }

} // namespace shotlet
