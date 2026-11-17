#include "scrollcapture/WinFrameSource.h"

#if !defined(SHOTLET_PLATFORM_WINDOWS)
#error "WinFrameSource.cpp 只能在 Windows 上编译"
#endif

#include "platform/PlatformWin32.h"
#include "utils/Logger.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace shotlet {

WinFrameSource::~WinFrameSource() {
    Stop();
}

bool WinFrameSource::Start(const GlobalRect& rect) {
    Stop();
    if (rect.w <= 0 || rect.h <= 0) {
        m_lastError = "滚动截图选区无效";
        return false;
    }
    RECT nativeRect{rect.x, rect.y, rect.x + rect.w, rect.y + rect.h};
    HMONITOR monitor = MonitorFromRect(&nativeRect, MONITOR_DEFAULTTONULL);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info) ||
        nativeRect.left < info.rcMonitor.left || nativeRect.top < info.rcMonitor.top ||
        nativeRect.right > info.rcMonitor.right || nativeRect.bottom > info.rcMonitor.bottom) {
        m_lastError = "滚动选区必须完整位于同一显示器内";
        return false;
    }

    HDC screenDc = GetDC(nullptr);
    HDC memoryDc = screenDc ? CreateCompatibleDC(screenDc) : nullptr;
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = rect.w;
    bitmapInfo.bmiHeader.biHeight = -rect.h;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;
    unsigned char* bits = nullptr;
    HBITMAP bitmap = memoryDc ? CreateDIBSection(memoryDc, &bitmapInfo, DIB_RGB_COLORS,
        reinterpret_cast<void**>(&bits), nullptr, 0) : nullptr;
    if (!screenDc || !memoryDc || !bitmap || !bits) {
        if (bitmap) DeleteObject(bitmap);
        if (memoryDc) DeleteDC(memoryDc);
        if (screenDc) ReleaseDC(nullptr, screenDc);
        m_lastError = "无法创建 Windows 滚动截图帧缓冲区";
        return false;
    }

    m_rect = rect;
    m_screenDc = screenDc;
    m_memoryDc = memoryDc;
    m_bitmap = bitmap;
    m_bits = bits;
    m_oldBitmap = SelectObject(memoryDc, bitmap);
    m_stopRequested.store(false);
    m_lastError.clear();
    Logger::Info("[WinScrollCapture][frame] source ready rect=({},{} {}x{}) monitor=({},{} {}x{})",
                 rect.x, rect.y, rect.w, rect.h, info.rcMonitor.left, info.rcMonitor.top,
                 info.rcMonitor.right - info.rcMonitor.left,
                 info.rcMonitor.bottom - info.rcMonitor.top);
    return true;
}

void WinFrameSource::Stop() {
    RequestStop();
    HDC memoryDc = static_cast<HDC>(m_memoryDc);
    if (memoryDc && m_oldBitmap) SelectObject(memoryDc, static_cast<HGDIOBJ>(m_oldBitmap));
    if (m_bitmap) DeleteObject(static_cast<HBITMAP>(m_bitmap));
    if (memoryDc) DeleteDC(memoryDc);
    if (m_screenDc) ReleaseDC(nullptr, static_cast<HDC>(m_screenDc));
    m_screenDc = m_memoryDc = m_bitmap = m_oldBitmap = nullptr;
    m_bits = nullptr;
}

void WinFrameSource::RequestStop() {
    m_stopRequested.store(true);
    m_waitCv.notify_all();
}

bool WinFrameSource::Capture(ScrollImage& image) {
    if (m_stopRequested.load() || !m_screenDc || !m_memoryDc || !m_bits) return false;
    if (!BitBlt(static_cast<HDC>(m_memoryDc), 0, 0, m_rect.w, m_rect.h,
                static_cast<HDC>(m_screenDc), m_rect.x, m_rect.y, SRCCOPY | CAPTUREBLT)) {
        m_lastError = "BitBlt 采集选区失败";
        return false;
    }
    image.width = m_rect.w;
    image.height = m_rect.h;
    image.rgba.resize(static_cast<size_t>(m_rect.w) * m_rect.h * 4);
    const size_t pixels = static_cast<size_t>(m_rect.w) * m_rect.h;
    for (size_t i = 0; i < pixels; ++i) {
        image.rgba[i * 4] = m_bits[i * 4 + 2];
        image.rgba[i * 4 + 1] = m_bits[i * 4 + 1];
        image.rgba[i * 4 + 2] = m_bits[i * 4];
        image.rgba[i * 4 + 3] = 255;
    }
    return true;
}

bool WinFrameSource::WaitForStableFrame(const ScrollImage& baseline, ScrollImage& image,
                                        bool& stable) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() +
        std::chrono::milliseconds(static_cast<int>(m_config.settleTimeoutMs));
    ScrollImage quietReference;
    auto quietSince = Clock::now();
    bool sawResponse = false;
    bool haveQuietReference = false;
    int polls = 0;
    float maxBaselineDiff = 0.0f;
    float lastBaselineDiff = 0.0f;
    float lastAdjacentDiff = 0.0f;
    const auto startedAt = Clock::now();

    while (!m_stopRequested.load()) {
        ScrollImage current;
        if (!Capture(current)) {
            Logger::Warning("[WinScrollCapture][frame] capture failed while settling polls={} error={}",
                            polls, m_lastError);
            return false;
        }
        ++polls;
        lastBaselineDiff = ComputeFrameDiffRatio(
            baseline.View(), current.View(), m_config.stability);
        maxBaselineDiff = std::max(maxBaselineDiff, lastBaselineDiff);
        const bool differsFromBaseline = lastBaselineDiff > m_config.stability.stableDiffRatio;
        if (differsFromBaseline) sawResponse = true;

        const auto now = Clock::now();
        lastAdjacentDiff = haveQuietReference ? ComputeFrameDiffRatio(
            quietReference.View(), current.View(), m_config.stability) : 0.0f;
        if (!haveQuietReference || lastAdjacentDiff > m_config.stability.stableDiffRatio) {
            quietReference = current;
            quietSince = now;
            haveQuietReference = true;
        } else if (sawResponse &&
                   now - quietSince >= std::chrono::milliseconds(
                       static_cast<int>(m_config.quietPeriodMs))) {
            image = std::move(current);
            stable = true;
            Logger::Info("[WinScrollCapture][frame] settled polls={} elapsedMs={} sawResponse={} maxBaselineDiff={:.6f} lastBaselineDiff={:.6f} lastAdjacentDiff={:.6f} stable={}",
                         polls, std::chrono::duration_cast<std::chrono::milliseconds>(now - startedAt).count(),
                         sawResponse, maxBaselineDiff, lastBaselineDiff, lastAdjacentDiff, stable);
            return true;
        }

        if (now >= deadline) {
            image = std::move(current);
            // 页面完全没有变化代表已到边界；持续变化则交给 controller 原地重试。
            stable = !sawResponse ||
                now - quietSince >= std::chrono::milliseconds(
                    static_cast<int>(m_config.quietPeriodMs));
            Logger::Info("[WinScrollCapture][frame] timeout polls={} elapsedMs={} sawResponse={} maxBaselineDiff={:.6f} lastBaselineDiff={:.6f} lastAdjacentDiff={:.6f} stable={}",
                         polls, std::chrono::duration_cast<std::chrono::milliseconds>(now - startedAt).count(),
                         sawResponse, maxBaselineDiff, lastBaselineDiff, lastAdjacentDiff, stable);
            return true;
        }
        std::unique_lock lock(m_waitMutex);
        m_waitCv.wait_for(lock,
            std::chrono::milliseconds(static_cast<int>(m_config.pollIntervalMs)),
            [this] { return m_stopRequested.load(); });
    }
    return false;
}

}  // namespace shotlet
