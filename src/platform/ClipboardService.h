#pragma once

#if defined(SHOTLET_PLATFORM_MACOS)
#include <CoreFoundation/CoreFoundation.h>
#include <memory>
#include <future>
#include <atomic>
#endif

namespace shotlet {

// 将 RGBA 图像数据编码为 PNG 并写入系统剪贴板
// 返回 true 表示成功
bool CopyImageToClipboard(const unsigned char* rgbaData, int width, int height);

#if defined(SHOTLET_PLATFORM_MACOS)
// PNG 编码可在后台执行，借用的 RGBA 必须存活到编码返回；剪贴板发布由主线程执行。
using ClipboardPng = std::shared_ptr<const __CFData>;
ClipboardPng EncodeClipboardPng(const unsigned char* rgbaData, int width, int height);
// 先声明本次 PNG，立即粘贴时按需等待后台结果；返回剪贴板版本，失败为 -1。
int DeclareClipboardPng(std::shared_future<ClipboardPng> png, std::shared_ptr<std::atomic<bool>> published = {});
bool IsClipboardPngCurrent(int version);
bool PublishClipboardPng(const ClipboardPng& png, int expectedVersion = -1);
#endif

} // namespace shotlet
