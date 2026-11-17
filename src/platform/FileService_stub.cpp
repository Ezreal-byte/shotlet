#include "FileService.h"

namespace shotlet {

void ActivateAppForSaveDialog() {}
std::filesystem::path ChooseExportDirectory(Language, TextId) { return {}; }
bool RevealInFileManager(const std::filesystem::path&) { return false; }
bool OpenFileWithDefaultApplication(const std::filesystem::path&) { return false; }
CaptureSourceInfo GetForegroundCaptureSource() { return {}; }

std::string SaveFileWithDialog(const unsigned char* rgbaData, int width, int height, Language language) {
    return "";
}

} // namespace shotlet
