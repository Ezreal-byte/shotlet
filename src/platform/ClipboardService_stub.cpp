#include "ClipboardService.h"
#include "utils/Logger.h"

namespace shotlet {

bool CopyImageToClipboard(const unsigned char* rgbaData, int width, int height) {
    Logger::Warning("CopyImageToClipboard not implemented for this platform");
    return false;
}

} // namespace shotlet
