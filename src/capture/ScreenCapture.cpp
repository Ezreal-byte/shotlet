#include "ScreenCapture.h"
#include "utils/Logger.h"
#include <utility>

#if defined(SHOTLET_PLATFORM_MACOS)
#include "backends/CaptureMacOS_SC.h"
#elif defined(SHOTLET_PLATFORM_WINDOWS)
#include "backends/CaptureWindows.h"
#include "platform/PlatformWin32.h"
#elif defined(SHOTLET_PLATFORM_LINUX)
#include "backends/CaptureLinux.h"
#endif

namespace shotlet {
    CaptureImage::~CaptureImage() {
        Free();
    }

    CaptureImage::CaptureImage(CaptureImage&& other) noexcept {
        *this = std::move(other);
    }

    CaptureImage& CaptureImage::operator=(CaptureImage&& other) noexcept {
        if (this == &other) {
            return *this;
        }

        Free();

        data = other.data;
        width = other.width;
        height = other.height;
        captureX = other.captureX;
        captureY = other.captureY;
        rowPitch = other.rowPitch;
        displayId = other.displayId;
        scaleFactor = other.scaleFactor;
        gdiHandle = other.gdiHandle;

        other.data = nullptr;
        other.width = 0;
        other.height = 0;
        other.captureX = 0;
        other.captureY = 0;
        other.rowPitch = 0;
        other.displayId = -1;
        other.scaleFactor = 1.0f;
        other.gdiHandle = nullptr;

        return *this;
    }

    void CaptureImage::Free() {
#if defined(SHOTLET_PLATFORM_WINDOWS)
        if (gdiHandle) {
            // GDI 分配的内存（Windows CreateDIBSection），使用 DeleteObject 释放
            DeleteObject(gdiHandle);
        } else if (data) {
            // 堆分配的内存，使用 delete[] 释放
            delete[] data;
        }
#else
        if (data) {
            delete[] data;
        }
#endif
        data = nullptr;
        gdiHandle = nullptr;
        width = height = rowPitch = 0;
    }

    std::unique_ptr<IScreenCapture> CreateScreenCapture() {
#if defined(SHOTLET_PLATFORM_MACOS)
        return std::make_unique<CaptureMacOS>();
#elif defined(SHOTLET_PLATFORM_WINDOWS)
        Logger::Info("Creating Windows screen capture");
        return std::make_unique<CaptureWindows>();
#elif defined(SHOTLET_PLATFORM_LINUX)
        Logger::Info("Creating Linux screen capture");
        return std::make_unique<CaptureLinux>();
#else
        Logger::Error("Unsupported platform");
        return nullptr;
#endif
    }
} // namespace shotlet
