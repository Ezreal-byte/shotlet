#pragma once
#include "capture/ScreenCapture.h"

#if defined(SHOTLET_PLATFORM_LINUX)

#include <string>
#include <vector>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

namespace shotlet {

class CaptureLinux : public IScreenCapture {
public:
    CaptureLinux();
    ~CaptureLinux() override;

    bool Initialize() override;
    std::vector<DisplayInfo> GetDisplays() override;
    CaptureImage Capture(const CaptureOptions& options) override;
    std::string GetLastErrorMessage() const override;

private:
    CaptureImage CaptureX11(const CaptureOptions& options);
    CaptureImage CaptureXDGPortal(const CaptureOptions& options);
    
    bool IsWayland();
    bool HasXDGPortal();
    
    std::string m_lastError;
    bool m_initialized = false;
    
    // X11
    Display* m_xDisplay = nullptr;
    Window m_xRootWindow = 0;
};

} // namespace shotlet

#endif // SHOTLET_PLATFORM_LINUX
