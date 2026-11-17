#include "platform/MacImageOutput.h"
#import <Foundation/Foundation.h>
#import <ImageIO/ImageIO.h>
#include <cstdio>

namespace shotlet {
namespace {
constexpr size_t kRgbaChannels = 4;
constexpr size_t kBitsPerChannel = 8;
constexpr size_t kBitsPerPixel = kRgbaChannels * kBitsPerChannel;

CGImageRef CreateRgbaImage(CGDataProviderRef provider, int width, int height, size_t rowPitchBytes,
                           CGImageAlphaInfo alphaInfo) {
    CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
    if (!colorSpace) return nullptr;
    CGImageRef image = CGImageCreate(width, height, kBitsPerChannel, kBitsPerPixel, rowPitchBytes, colorSpace,
        static_cast<CGBitmapInfo>(alphaInfo) | kCGBitmapByteOrder32Big,
        provider, nullptr, false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(colorSpace);
    return image;
}
} // namespace

CGImageRef CreateImageFromRGBA(const unsigned char* rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0) return nullptr;
    const size_t pitch = size_t(width) * kRgbaChannels;
    CGDataProviderRef provider = CGDataProviderCreateWithData(nullptr, rgba, pitch * height, nullptr);
    if (!provider) return nullptr;
    CGImageRef image = CreateRgbaImage(provider, width, height, pitch, kCGImageAlphaPremultipliedLast);
    CGDataProviderRelease(provider);
    return image;
}

CGImageRef CreateOwnedImageFromRGBA(const unsigned char* rgba, int width, int height, int rowPitchBytes) {
    if (!rgba || width <= 0 || height <= 0 || rowPitchBytes <= 0 ||
        size_t(rowPitchBytes) < size_t(width) * kRgbaChannels) return nullptr;
    // CFData 持有独立副本，provider 再保留 CFData；SDL_Surface 释放后延迟绘制仍然安全。
    const CFIndex byteCount = CFIndex(rowPitchBytes) * height;
    CFDataRef pixels = CFDataCreate(kCFAllocatorDefault, rgba, byteCount);
    if (!pixels) return nullptr;
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(pixels);
    CFRelease(pixels);
    if (!provider) return nullptr;
    // SDL RGBA32 的 RGB 尚未乘 Alpha，保留正确的透明边缘语义。
    CGImageRef image = CreateRgbaImage(provider, width, height, size_t(rowPitchBytes), kCGImageAlphaLast);
    CGDataProviderRelease(provider);
    return image;
}
bool WritePngToFile(const unsigned char* rgba, int width, int height, const std::string& path) {
    @autoreleasepool {
        CGImageRef image = CreateImageFromRGBA(rgba, width, height);
        if (!image || path.empty()) { if (image) CGImageRelease(image); return false; }
        NSString* target = [NSString stringWithUTF8String:path.c_str()];
        NSString* temporary = [target stringByAppendingFormat:@".%@.tmp", NSUUID.UUID.UUIDString];
        CGImageDestinationRef destination = CGImageDestinationCreateWithURL(
            (__bridge CFURLRef)[NSURL fileURLWithPath:temporary], CFSTR("public.png"), 1, nullptr);
        if (!destination) { CGImageRelease(image); return false; }
        CGImageDestinationAddImage(destination, image, nullptr);
        bool ok = CGImageDestinationFinalize(destination);
        CFRelease(destination);
        CGImageRelease(image);
        if (ok) ok = std::rename(temporary.fileSystemRepresentation, target.fileSystemRepresentation) == 0;
        if (!ok) [[NSFileManager defaultManager] removeItemAtPath:temporary error:nil];
        return ok;
    }
}
} // namespace shotlet
