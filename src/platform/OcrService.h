#pragma once

#include <future>
#include <string>

namespace shotlet {

// OCR 文字识别服务
// macOS 使用 Vision 框架（VNRecognizeTextRequest）实现中英文识别
struct OcrResult { bool success = false; std::string text; std::string error; };

class OcrService {
public:
    static std::future<OcrResult> RecognizeDetailedAsync(const unsigned char* rgbaData, int width, int height);
    // 从 RGBA 图像数据识别文字
    // rgbaData: RGBA 像素数据（行序，4字节/像素）
    // width/height: 像素尺寸
    // 返回识别文本字符串（空串表示未识别到文字）
    static std::string Recognize(const unsigned char* rgbaData, int width, int height);

    // 异步识别：在后台线程执行，调用方通过 future 轮询/等待结果。
    // Vision Accurate 级别识别可能耗时数百毫秒，避免阻塞主循环。
    // 注意：std::async future 析构会等待线程结束，持有者退出前需自行收尾。
    static std::future<std::string> RecognizeAsync(const unsigned char* rgbaData, int width, int height);
};

} // namespace shotlet