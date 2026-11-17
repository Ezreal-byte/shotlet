#include "OcrWindow.h"
#include "utils/Logger.h"

namespace shotlet {

bool OcrWindow::Create(const unsigned char* rgbaData, int width, int height,
                       const std::string& text,
                       float screenX, float screenY,
                       float scaleFactor, Language language, Theme theme) {
    Logger::Warning("[OcrWindow] OCR window not supported on this platform");
    return false;
}

void OcrWindow::SetTheme(Theme) {}

void OcrWindow::Destroy() {}
bool OcrWindow::IsVisible() { return false; }
void OcrWindow::ProcessEvents() {}

} // namespace shotlet
