#include "MagnifierOverlay.h"
#include "utils/Logger.h"

#if !defined(SHOTLET_PLATFORM_MACOS)
#if !defined(SHOTLET_PLATFORM_WINDOWS)

shotlet::MagnifierOverlay::MagnifierOverlay()
    : m_impl(nullptr) {
}

shotlet::MagnifierOverlay::~MagnifierOverlay() {
    Shutdown();
}

bool shotlet::MagnifierOverlay::Initialize() {
    Logger::Warning("MagnifierOverlay not supported on this platform");
    return false;
}

void shotlet::MagnifierOverlay::Shutdown() {
}

void shotlet::MagnifierOverlay::Show() {
    Logger::Warning("MagnifierOverlay not supported on this platform");
}

void shotlet::MagnifierOverlay::Hide() {
}

void shotlet::MagnifierOverlay::UpdateMousePosition(int x, int y) {
}

void shotlet::MagnifierOverlay::RefreshMousePosition() {
}

void shotlet::MagnifierOverlay::SetScreenData(std::vector<ScreenData>&& screens) {
}

void shotlet::MagnifierOverlay::ClearScreenData() {
}

void shotlet::MagnifierOverlay::SetScreenshotData(int width, int height, const uint8_t* data, int rowPitch, int x, int y, float scaleFactor) {
}

void shotlet::MagnifierOverlay::UpdateScreenshot() {
}

const shotlet::ScreenData* shotlet::MagnifierOverlay::GetScreenData(int displayId) const {
    return nullptr;
}

void shotlet::MagnifierOverlay::CopyColorToClipboard() {
}

#endif
#endif
