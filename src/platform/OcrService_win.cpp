#include "OcrService.h"

#if !defined(SHOTLET_PLATFORM_WINDOWS)
#error "OcrService_win.cpp must only be compiled on Windows"
#endif

#include "platform/PlatformWin32.h"
#include "utils/Logger.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Storage.Streams.h>

#include <cstring>
#include <future>
#include <string>
#include <utility>
#include <vector>

namespace shotlet {
namespace {

bool EnsureWinRTApartment() {
    static thread_local bool initialized = false;
    if (initialized) {
        return true;
    }

    try {
        // MTA：OCR 的 WinRT 调用在专用后台线程上同步等待（.get()）时，
        // 完成回调可在 RPC 线程池线程派发，不会像 STA 一样死锁。
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        initialized = true;
        return true;
    } catch (const winrt::hresult_error& ex) {
        if (ex.code() == RPC_E_CHANGED_MODE) {
            Logger::Warning("[OcrService] COM already initialized with a different apartment model; continuing");
            initialized = true;
            return true;
        }

        Logger::Error("[OcrService] WinRT apartment initialization failed: {}",
                      winrt::to_string(ex.message()));
        return false;
    }
}

} // namespace

static OcrResult RecognizeDetailed(const unsigned char* rgbaData, int width, int height) {
    if (!rgbaData || width <= 0 || height <= 0) {
        Logger::Error("[OcrService] Invalid parameters");
        return {false, {}, "OCR 识别失败或引擎不可用"};
    }

    try {
        if (!EnsureWinRTApartment()) {
            return {false, {}, "OCR 识别失败或引擎不可用"};
        }

        // RGBA → BGRA 转换
        size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
        std::vector<uint8_t> bgraData(pixelCount * 4);
        for (size_t i = 0; i < pixelCount; ++i) {
            bgraData[i * 4 + 0] = rgbaData[i * 4 + 2];  // B ← R
            bgraData[i * 4 + 1] = rgbaData[i * 4 + 1];  // G ← G
            bgraData[i * 4 + 2] = rgbaData[i * 4 + 0];  // R ← B
            bgraData[i * 4 + 3] = rgbaData[i * 4 + 3];  // A ← A
        }

        // 通过 DataWriter 创建 IBuffer
        winrt::Windows::Storage::Streams::DataWriter writer;
        writer.WriteBytes(winrt::array_view<uint8_t const>(bgraData));
        auto buffer = writer.DetachBuffer();

        // 创建 SoftwareBitmap 并从 Buffer 复制像素数据
        winrt::Windows::Graphics::Imaging::SoftwareBitmap bitmap(
            winrt::Windows::Graphics::Imaging::BitmapPixelFormat::Bgra8,
            width, height);
        bitmap.CopyFromBuffer(buffer);

        // 创建 OCR 引擎（支持中文和英文）
        auto engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromLanguage(
            winrt::Windows::Globalization::Language{L"zh-Hans"});

        if (!engine) {
            // 如果中文引擎不可用，尝试英文
            engine = winrt::Windows::Media::Ocr::OcrEngine::TryCreateFromLanguage(
                winrt::Windows::Globalization::Language{L"en-US"});
        }

        if (!engine) {
            Logger::Error("[OcrService] No OCR engine available");
            return {false, {}, "OCR 识别失败或引擎不可用"};
        }

        // 同步执行 OCR 识别
        auto result = engine.RecognizeAsync(bitmap).get();

        // 拼接识别文本
        std::string text;
        for (const auto& line : result.Lines()) {
            if (!text.empty()) {
                text += "\n";
            }
            text += winrt::to_string(line.Text());
        }

        Logger::Info("[OcrService] Recognized {} characters", text.size());
        return {true, std::move(text), {}};

    } catch (const winrt::hresult_error& ex) {
        Logger::Error("[OcrService] WinRT error: {}", winrt::to_string(ex.message()));
        return {false, {}, "OCR 识别失败或引擎不可用"};
    } catch (const std::exception& ex) {
        Logger::Error("[OcrService] Exception: {}", ex.what());
        return {false, {}, "OCR 识别失败或引擎不可用"};
    }
}

std::future<OcrResult> OcrService::RecognizeDetailedAsync(const unsigned char* data, int w, int h) {
    if (!data || w <= 0 || h <= 0 || size_t(w) > 256u * 1024u * 1024u / 4 / size_t(h)) {
        std::promise<OcrResult> ready; ready.set_value({false, {}, "OCR 图像参数无效"}); return ready.get_future();
    }
    std::vector<unsigned char> pixels(data, data + size_t(w) * h * 4);
    return std::async(std::launch::async, [pixels = std::move(pixels), w, h] { return RecognizeDetailed(pixels.data(), w, h); });
}
std::future<std::string> OcrService::RecognizeAsync(const unsigned char* data, int w, int h) {
    auto future = RecognizeDetailedAsync(data, w, h);
    return std::async(std::launch::async, [future = std::move(future)]() mutable { return future.get().text; });
}
std::string OcrService::Recognize(const unsigned char* data, int w, int h) { return RecognizeDetailedAsync(data, w, h).get().text; }
} // namespace shotlet
