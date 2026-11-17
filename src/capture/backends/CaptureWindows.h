#pragma once
#include "capture/ScreenCapture.h"

#if defined(SHOTLET_PLATFORM_WINDOWS)

// Do NOT include windows.h here!
// It defines macros that conflict with our method names.
// Use void* instead of HMONITOR to avoid windows.h dependency in the header.

#include <string>
#include <vector>

namespace shotlet {

class CaptureWindows : public IScreenCapture {
public:
    CaptureWindows();
    ~CaptureWindows() override;

    bool Initialize() override;
    std::vector<DisplayInfo> GetDisplays() override;
    CaptureImage Capture(const CaptureOptions& options) override;
    std::string GetLastErrorMessage() const override;

private:
    CaptureImage CaptureGDI(const CaptureOptions& options);
    // void* instead of HMONITOR to avoid windows.h in header
    CaptureImage CaptureDisplayHMONITOR(void* hMonitor, bool includeCursor);

    std::string m_lastError;
    bool m_initialized = false;
};

} // namespace shotlet

#endif // SHOTLET_PLATFORM_WINDOWS
