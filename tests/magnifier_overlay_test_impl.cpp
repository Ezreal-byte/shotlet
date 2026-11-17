// Test-only minimal implementation of MagnifierOverlay
// Does not depend on any platform-specific APIs

#include "platform/MagnifierOverlay.h"

// Define Impl for test builds (real builds define it in MagnifierOverlay_mac.mm)
struct shotlet::MagnifierOverlay::Impl {};

namespace shotlet {

    MagnifierOverlay::MagnifierOverlay() : m_impl(std::make_unique<Impl>()) {}
    MagnifierOverlay::~MagnifierOverlay() { Shutdown(); }

    bool MagnifierOverlay::Initialize() { return true; }
    void MagnifierOverlay::Shutdown() {}

    void MagnifierOverlay::Show() { m_visible = true; }
    void MagnifierOverlay::Hide() { m_visible = false; }

    void MagnifierOverlay::UpdateMousePosition(int x, int y) {
        m_mouseX = x;
        m_mouseY = y;
    }
    void MagnifierOverlay::RefreshMousePosition() {
        UpdateMousePosition(123, 456);
    }
    void MagnifierOverlay::SetScreenData(std::vector<ScreenData>&& screens) { m_screens = std::move(screens); }
    void MagnifierOverlay::ClearScreenData() { m_screens.clear(); }
    void MagnifierOverlay::SetScreenshotData(int, int, const uint8_t*, int, int, int, float) {}
    void MagnifierOverlay::UpdateScreenshot() {}

    const ScreenData* MagnifierOverlay::GetScreenData(int displayId) const {
        for (const auto& sd : m_screens) {
            if (sd.displayId == displayId) return &sd;
        }
        return nullptr;
    }

    void MagnifierOverlay::CopyColorToClipboard() {}

} // namespace shotlet
