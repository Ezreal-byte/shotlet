#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <memory>
#include <functional>
#include <string>
#include <vector>

#include "scrollcapture/FrameStability.h"

namespace shotlet {

// 采集输出帧：RGBA 紧凑排列（pitch == width*4），独占或不可变共享像素。
struct CapturedFrame {
    std::vector<uint8_t> pixels;
    std::shared_ptr<const std::vector<uint8_t>> sharedPixels;
    int width = 0;
    int height = 0;
    uint64_t frameId = 0;  // Complete/Idle 样本的递增序号
    uint64_t displayTime = 0;  // 样本生成时的 mach absolute time，不是回调到达时间
    bool stable = false;   // settle 超时取最后帧时为 false（喂给 Stitcher 的 FrameInput.stable）
    PixelView View() const { return {sharedPixels ? sharedPixels->data() : pixels.data(), width, height, width * 4}; }
};

// 在注入前记录画面和时间，后续帧必须晚于此时间才属于当前步骤。
struct CaptureBaseline {
    CapturedFrame frame;
    uint64_t notBefore = 0;
};

// 持续流帧采集（设计 10.1 MacFrameSource）：SCStream 封装。
// 线程模型：帧回调在 SCK 队列线程，内部 mutex + condition_variable 同步，
// WaitFor* 方法由调用方（demo 主线程 / M3 主循环）阻塞等待。
class MacFrameSource {
public:
    struct Config {
        double settleTimeoutMs = 400.0;  // 稳定等待上限（设计 10.4 建议值）
        double quietPeriodMs = 80.0;     // 看到实际响应后，要求持续静止；不叠加固定延迟
        FrameStabilityConfig stability;
    };

    // 启动持续流。displayId 为 CGDirectDisplayID；rect 为全局坐标点选区；
    // excludeOwnWindows=true 时按 owningApplication.processID==getpid() 排除自身窗口。
    bool Start(uint32_t cgDisplayId, const GlobalRect& rect, bool excludeOwnWindows, const std::function<bool()>& cancelled = {});
    void Stop();
    void RequestStop();
    [[nodiscard]] bool IsRunning() const;

    // 当前最新样本序号（0 = 尚无帧）。必须在注入前取基线
    [[nodiscard]] uint64_t CurrentFrameId() const;
    [[nodiscard]] CaptureBaseline TakeBaseline() const;

    // 取序号严格大于 afterFrameId 的下一帧（首帧采集用），超时返回 false
    bool WaitForNextFrame(uint64_t afterFrameId, CapturedFrame& out, double timeoutMs);
    // 返回不可变帧引用，不复制像素；连续匹配只保留正在处理的帧与最新帧。
    bool WaitForFrameReference(uint64_t afterFrameId, CapturedFrame& out, double timeoutMs);

    // 先观察滚动响应，再等待持续静止；无变化必须等满 settleTimeoutMs。
    // 单个 Idle 不是步骤完成信号。超时仍在变化时只返回不稳定帧，不用于标定。
    bool WaitForStableFrame(const CaptureBaseline& baseline, CapturedFrame& out);

    [[nodiscard]] std::string GetLastErrorMessage() const {
        std::lock_guard lock(m_frameMutex);
        return m_lastError;
    }

    // 内部接口（.mm 的 SCStream 回调调用；不对外）
    void HandleFrame(const uint8_t* data, int width, int height, int pitch, bool isBGRA, uint64_t displayTime = 0);
    void HandleIdleFrame(uint64_t displayTime = 0);
    void HandleStreamError(const char* message);

    MacFrameSource() = default;
    ~MacFrameSource();
    MacFrameSource(const MacFrameSource&) = delete;
    MacFrameSource& operator=(const MacFrameSource&) = delete;

private:
    // ObjC 对象桥接（照抄 CaptureMacOS_SC.h 的 void* 手法，头文件保持纯 C++）
    void* m_stream = nullptr;   // SCStream*
    void* m_output = nullptr;   // MacFrameSourceStreamOutput*
    void* m_queue = nullptr;    // dispatch_queue_t

    Config m_config;
    uint32_t m_displayId = 0;
    GlobalRect m_rect{};

    mutable std::mutex m_frameMutex;  // 保护以下状态（SCK 回调线程 vs 调用方线程）
    std::condition_variable m_frameCv;

    std::shared_ptr<const std::vector<uint8_t>> m_latestPixels;
    int m_frameWidth = 0;                 // 输出帧像素尺寸（HandleFrame 记录）
    int m_frameHeight = 0;
    uint64_t m_latestFrameId = 0;
    uint64_t m_latestDisplayTime = 0;
    bool m_latestIdle = false;
    bool m_streamFailed = false;
    std::string m_lastError;
};

}  // namespace shotlet
