#include "OcrService.h"
#include "utils/Logger.h"

namespace shotlet {
std::future<OcrResult> OcrService::RecognizeDetailedAsync(const unsigned char*, int, int) {
    std::promise<OcrResult> ready; ready.set_value({false, {}, "当前系统不支持 OCR"}); return ready.get_future();
}


std::string OcrService::Recognize(const unsigned char* rgbaData, int width, int height) {
    Logger::Warning("[OcrService] OCR not supported on this platform");
    return "";
}

std::future<std::string> OcrService::RecognizeAsync(const unsigned char* rgbaData, int width, int height) {
    std::promise<std::string> ready;
    ready.set_value(Recognize(rgbaData, width, height));
    return ready.get_future();
}

} // namespace shotlet