#import <Vision/Vision.h>
#import <Cocoa/Cocoa.h>
#import <CoreGraphics/CoreGraphics.h>

#include "OcrService.h"
#include "utils/Logger.h"
#include "output/LongImageLayout.h"

#include <thread>
#include <utility>
#include <vector>

namespace shotlet {

namespace {

// Vision 识别逻辑（在工作线程执行，所有 Vision 对象在该线程创建和消费）
OcrResult RecognizeOnWorker(std::vector<unsigned char> imageData, int width, int height) {
    std::string resultText;
    // 每块上下重叠，按文本框中心所在的归属区间去重；相同文本在不同位置仍保留。
    for (const auto& slice : PlanOcrSlices(height)) {
        @autoreleasepool {
            CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
            CGBitmapInfo info = static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big;
            CGContextRef ctx = CGBitmapContextCreate(imageData.data() + size_t(slice.top) * width * 4,
                width, slice.height, 8, width * 4, colorSpace, info);
            CGColorSpaceRelease(colorSpace);
            if (!ctx) return {false, {}, "无法创建 OCR 图像上下文"};
            CGImageRef image = CGBitmapContextCreateImage(ctx);
            CGContextRelease(ctx);
            if (!image) return {false, {}, "无法创建 OCR 图像"};
            VNImageRequestHandler* handler = [[VNImageRequestHandler alloc] initWithCGImage:image options:@{}];
            VNRecognizeTextRequest* request = [[VNRecognizeTextRequest alloc] init];
            request.recognitionLevel = VNRequestTextRecognitionLevelAccurate;
            request.recognitionLanguages = @[@"zh-Hans", @"en-US"];
            request.usesLanguageCorrection = YES;
            NSError* error = nil;
            if (![handler performRequests:@[request] error:&error]) {
                CGImageRelease(image);
                const char* message = error.localizedDescription.UTF8String;
                Logger::Error("[OcrService] Vision request failed: {}", message ? message : "Vision 返回失败但没有错误详情");
                return {false, {}, message ? message : "Vision 识别失败"};
            }
            for (VNRecognizedTextObservation* observation in request.results) {
                const double centerY = slice.top + (1.0 - CGRectGetMidY(observation.boundingBox)) * slice.height;
                if (centerY < slice.acceptTop || centerY >= slice.acceptBottom) continue;
                VNRecognizedText* text = [observation topCandidates:1].firstObject;
                if (text.string.length) {
                    if (!resultText.empty()) resultText += "\n";
                    resultText += text.string.UTF8String;
                }
            }
            CGImageRelease(image);
        }
    }
    return {true, std::move(resultText), {}};
}

} // namespace

std::future<OcrResult> OcrService::RecognizeDetailedAsync(const unsigned char* rgbaData, int width, int height) {
    if (!rgbaData || width <= 0 || height <= 0 || size_t(width) > 256u * 1024u * 1024u / 4 / size_t(height)) {
        Logger::Error("[OcrService] Invalid image data");
        std::promise<OcrResult> ready;
        ready.set_value({false, {}, "OCR 图像参数无效"});
        return ready.get_future();
    }

    const size_t dataSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    std::vector<unsigned char> imageData(rgbaData, rgbaData + dataSize);

    // Keep Vision off Electron's AppKit main thread: std::launch::async 保证
    // 在独立工作线程执行，结果通过 future 交回调用方。
    return std::async(std::launch::async,
                      [imageData = std::move(imageData), width, height]() mutable {
                          return RecognizeOnWorker(std::move(imageData), width, height);
                      });
}

std::future<std::string> OcrService::RecognizeAsync(const unsigned char* data, int w, int h) {
    auto future = RecognizeDetailedAsync(data, w, h);
    return std::async(std::launch::async, [future = std::move(future)]() mutable { return future.get().text; });
}
std::string OcrService::Recognize(const unsigned char* data, int w, int h) { return RecognizeDetailedAsync(data, w, h).get().text; }
} // namespace shotlet
