#pragma once

#include "scrollcapture/ScrollCaptureStitcher.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace shotlet {

struct ScrollImage {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    std::shared_ptr<const std::vector<uint8_t>> sharedRgba;
    PixelView View() const { return {sharedRgba ? sharedRgba->data() : rgba.data(), width, height, width * 4}; }
};

enum class ScrollCaptureState { Idle, Preparing, Capturing, Finalizing, Failed, Done, Cancelled };

// 采集/验证由控制线程调用；连续模式可独立调度注入，Stop 必须等待其退出。
class ScrollCaptureBackend {
public:
    virtual ~ScrollCaptureBackend() = default;
    virtual bool Start(ScrollImage& first, std::function<void(int64_t)> intent,
                       std::string& error) = 0;
    virtual bool CaptureStep(int64_t inject, ScrollImage& image, bool& stable,
                             std::string& error) = 0;
    virtual bool ValidateTarget(std::string& error) = 0;
    // 清空本步暂存输入；实现可能回调控制器，调用时不能持有控制器锁。
    virtual void DiscardPendingInput() {}
    virtual bool CancelRequested() const { return false; }
    virtual void RequestCancel() {} // 可从主线程调用，必须无阻塞
    // 连续模式由平台独立调度滚动，控制器只消费新帧并反馈真实位移。
    virtual bool ContinuousInput() const { return false; }
    virtual void ConfigureContinuousInput(int, int) {}
    virtual void PauseInput() {} // Finish/Cancel 可跨线程调用，必须无阻塞
    // 后端确认输入线程已暂停，且输入之后的最新帧已经交给控制器。
    // 无法证明时保持 false，继续使用有界收尾等待。
    // 契约：调用方持有控制器互斥量（m_mutex）后同步调用本方法，
    // 实现不得获取控制器锁（或任何反向持有 m_frameMutex 再回调控制器的锁），必须快速返回。
    virtual bool FinalFrameSettled() const { return false; }
    virtual void FrameFeedback(const StitchResult&, bool, int = 0) {}
    virtual void Stop() = 0;
};

struct ScrollCaptureSnapshot {
    uint64_t epoch = 0, revision = 0;
    uint64_t imageVersion = 0; // 独立于方向提示版本，拒绝迟到预览
    ScrollCaptureState state = ScrollCaptureState::Idle;
    std::string message;
    std::shared_ptr<const ScrollImage> viewport, preview, result;
    int totalHeight = 0;
    bool expandedTop = false;
    bool reverseScrollBlocked = false; // 反向输入已拦截，界面显示红色提示
};

class ScrollCaptureController {
public:
    explicit ScrollCaptureController(Language language = Language::English) : m_language(language) {}
    ~ScrollCaptureController();
    bool Start(std::unique_ptr<ScrollCaptureBackend> backend, const StitcherConfig& limits = {});
    void AddIntent(uint64_t epoch, int64_t delta);
    void Finish();
    void Cancel();
    ScrollCaptureSnapshot Snapshot() const;
    bool IsStopped() const;
    void Join();
private:
    void Run(uint64_t epoch, std::unique_ptr<ScrollCaptureBackend> backend, StitcherConfig limits);
    bool Current(uint64_t epoch) const;
    void Publish(uint64_t epoch, ScrollCaptureSnapshot snapshot);
    // 调用方必须持有 m_mutex，统一保留方向提示、预览和会话失效规则。
    void PublishLocked(uint64_t epoch, ScrollCaptureSnapshot snapshot);
    void CompleteStep(uint64_t epoch, ScrollCaptureBackend& backend, ScrollCaptureSnapshot snapshot);
    void PublishPreview(uint64_t epoch, uint64_t version, std::shared_ptr<const ScrollImage> preview);
    void RunContinuous(uint64_t epoch, ScrollCaptureBackend& backend,
                       ScrollCaptureStitcher& stitcher, ScrollCaptureSnapshot& state,
                       const StitcherConfig& limits,
                       const std::function<void()>& schedulePreview);
    const Language m_language;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::thread m_worker;
    uint64_t m_epoch = 0;
    bool m_cancel = false, m_finish = false, m_exited = true;
    int64_t m_pending = 0, m_stepPixels = 1;
    bool m_stepInFlight = false; // 取帧和拼接期间不积压触发；步骤完成时与快照在同一临界区更新。
    int m_direction = 0; // 首次有效滚动锁定方向；1 向下，-1 向上
    std::chrono::steady_clock::time_point m_finishDeadline;
    std::chrono::steady_clock::time_point m_finishStarted;
    ScrollCaptureSnapshot m_snapshot;
    ScrollCaptureBackend* m_backend = nullptr; // 受 m_mutex 保护，仅用于取消通知
};

// 从分块内容生成有界缩略图，不分配整张长图的连续副本。
ScrollImage MakeScrollPreview(const LongImageStore& store, int maxWidth = 180, int maxHeight = 600, bool showTop = false);

} // namespace shotlet
