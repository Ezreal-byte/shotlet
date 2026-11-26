#pragma once
#include "capture/ScreenCapture.h"

#if defined(SHOTLET_PLATFORM_LINUX)
namespace shotlet {
    class CaptureLinux : public IScreenCapture {
    public:
        CaptureLinux();
        ~CaptureLinux() override;
        bool Initialize() override;
        std::vector<DisplayInfo> GetDisplays() override;
        CaptureImage Capture(const CaptureOptions &options) override;
        std::string GetLastErrorMessage() const override;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_impl;
        std::string m_lastError;
    };
} // namespace shotlet
#endif
