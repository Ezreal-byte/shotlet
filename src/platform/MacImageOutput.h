#pragma once
#if defined(SHOTLET_PLATFORM_MACOS)
#include <CoreGraphics/CoreGraphics.h>
#include <string>
namespace shotlet {
// 借用 RGBA；直到返回的 CGImage 及其使用方全部释放前，调用方都必须保留像素。
CGImageRef CreateImageFromRGBA(const unsigned char* rgba, int width, int height);
// 复制未预乘的 RGBA（允许行尾填充），供 NSImage 等延迟绘制对象长期持有。
CGImageRef CreateOwnedImageFromRGBA(const unsigned char* rgba, int width, int height, int rowPitchBytes);
// 同步借用 RGBA，调用结束后即可释放；文件导出不额外持有整张 PNG 内存。
bool WritePngToFile(const unsigned char* rgba, int width, int height, const std::string& path);
} // namespace shotlet
#endif
