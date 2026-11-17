#pragma once
#include "utils/ScopeExit.h"
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>
#include <filesystem>
#include <string>
#include <vector>

namespace shotlet::TestUtils {
// 强制栅格化系统解码结果，不能仅凭创建了惰性 CGImage 就判断压缩数据有效。
inline std::vector<uint8_t> DecodeImageFrame(const std::filesystem::path& path, size_t index,
                                            int& width, int& height, std::string& error) {
    const auto native = path.native();
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(native.data()), native.size(), false);
    if (!url) { error = "图像路径无效"; return {}; }
    const ScopeExit releaseUrl([&] { CFRelease(url); });
    CGImageSourceRef source = CGImageSourceCreateWithURL(url, nullptr);
    if (!source) { error = "无法读取图像"; return {}; }
    const ScopeExit releaseSource([&] { CFRelease(source); });
    CGImageRef image = CGImageSourceCreateImageAtIndex(source, index, nullptr);
    if (!image) { error = "无法解码图像帧"; return {}; }
    const ScopeExit releaseImage([&] { CGImageRelease(image); });
    width = int(CGImageGetWidth(image)); height = int(CGImageGetHeight(image));
    constexpr int Channels = 4, BitsPerChannel = 8;
    std::vector<uint8_t> pixels(size_t(width) * height * Channels);
    CGColorSpaceRef color = CGColorSpaceCreateDeviceRGB();
    const ScopeExit releaseColor([&] { CGColorSpaceRelease(color); });
    CGContextRef context = CGBitmapContextCreate(pixels.data(), width, height, BitsPerChannel, width * Channels, color,
        CGBitmapInfo(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
    if (!context) { error = "无法创建图像解码画布"; return {}; }
    const ScopeExit releaseContext([&] { CGContextRelease(context); });
    CGContextDrawImage(context, CGRectMake(0, 0, width, height), image);
    return pixels;
}
}
