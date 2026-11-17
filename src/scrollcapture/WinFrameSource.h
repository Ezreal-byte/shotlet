#pragma once

#include "scrollcapture/FrameStability.h"
#include "scrollcapture/ScrollCaptureController.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>

namespace shotlet {

// Windows 选区轮询帧源。GDI 对象在单个 controller 工作线程中使用。
class WinFrameSource {
public:
    struct Config {
        double settleTimeoutMs = 400.0;
        double quietPeriodMs = 80.0;
        double pollIntervalMs = 16.0;
        FrameStabilityConfig stability;
    };

    WinFrameSource() = default;
    ~WinFrameSource();
    WinFrameSource(const WinFrameSource&) = delete;
    WinFrameSource& operator=(const WinFrameSource&) = delete;

    bool Start(const GlobalRect& rect);
    void Stop();
    void RequestStop();
    bool Capture(ScrollImage& image);
    bool WaitForStableFrame(const ScrollImage& baseline, ScrollImage& image, bool& stable);

    [[nodiscard]] const std::string& GetLastErrorMessage() const { return m_lastError; }

private:
    GlobalRect m_rect{};
    void* m_screenDc = nullptr;
    void* m_memoryDc = nullptr;
    void* m_bitmap = nullptr;
    void* m_oldBitmap = nullptr;
    unsigned char* m_bits = nullptr;
    Config m_config;
    std::atomic<bool> m_stopRequested{false};
    std::mutex m_waitMutex;
    std::condition_variable m_waitCv;
    std::string m_lastError;
};

}  // namespace shotlet
