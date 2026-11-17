#include "PinWindow.h"
#include "utils/Logger.h"

namespace shotlet {

bool PinWindow::Create(const unsigned char* rgbaData, int width, int height,
                       float screenX, float screenY,
                       float scaleFactor, Language language, Theme theme) {
    (void)rgbaData;
    (void)width;
    (void)height;
    (void)screenX;
    (void)screenY;
    (void)scaleFactor;
    Logger::Warning("[PinWindow] Pin window not supported on this platform");
    return false;
}

void PinWindow::SetTheme(Theme) {}

void PinWindow::Destroy() {}

bool PinWindow::IsVisible() { return false; }

void PinWindow::ProcessEvents() {}

} // namespace shotlet
