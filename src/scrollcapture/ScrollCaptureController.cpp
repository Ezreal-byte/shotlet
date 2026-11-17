#include "scrollcapture/ScrollCaptureController.h"
#include "utils/Logger.h"
#include "scrollcapture/FrameStability.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <utility>
#include <optional>

namespace shotlet {
namespace {
// 单槽队列合并过期预览；快照共享不可变像素，追加与缩略图生成互不阻塞。
class PreviewWorker {
public:
    using Publish = std::function<void(uint64_t, std::shared_ptr<const ScrollImage>)>;
    explicit PreviewWorker(Publish publish) : m_publish(std::move(publish)), m_thread([this] { Run(); }) {}
    ~PreviewWorker() {
        { std::lock_guard lock(m_mutex); m_stop = true; }
        m_wake.notify_all();
        m_thread.join();
    }
    void Submit(const LongImageStore& store, uint64_t version, bool top) {
        auto snapshot = store.Snapshot();
        { std::lock_guard lock(m_mutex); m_store = std::move(snapshot); m_version = version; m_top = top; }
        m_wake.notify_one();
    }
private:
    void Run() {
        auto next = std::chrono::steady_clock::now();
        for (;;) {
            std::unique_lock lock(m_mutex);
            m_wake.wait(lock, [this] { return m_stop || m_store; });
            if (m_stop && !m_store) return;
            m_wake.wait_until(lock, next, [this] { return m_stop; });
            auto store = std::exchange(m_store, {});
            const auto version = m_version;
            const bool top = m_top;
            lock.unlock();
            const auto start = std::chrono::steady_clock::now();
            try {
                auto preview = std::make_shared<ScrollImage>(MakeScrollPreview(*store, 180, 600, top));
                m_publish(version, std::move(preview));
            } catch (const std::exception& e) {
                Logger::Warning("[ScrollCapture] preview failed: {}", e.what());
            }
            next = std::chrono::steady_clock::now() + std::chrono::milliseconds(66);
            Logger::Debug("[ScrollCapture] preview version={} elapsedMs={:.2f}", version,
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        }
    }
    Publish m_publish;
    std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stop = false, m_top = false;
    uint64_t m_version = 0;
    std::shared_ptr<const LongImageStore> m_store;
    std::thread m_thread;
};
}

ScrollImage MakeScrollPreview(const LongImageStore& store, int maxWidth, int maxHeight, bool showTop) {
    ScrollImage image;
    if (store.Width() <= 0 || store.Height() <= 0 || maxWidth <= 0 || maxHeight <= 0) return image;
    double ratio = std::min({1.0, double(maxWidth) / store.Width(), double(maxHeight) / store.Height()});
    // 等比缩小后过窄时保留最小宽度，展示最近扩展端，计数仍显示完整高度。
    const int minWidth = std::min({64, maxWidth, store.Width()});
    ratio = std::max(ratio, double(minWidth) / store.Width());
    const int visibleRows = std::min(store.Height(), std::max(1, int(maxHeight / ratio)));
    const int firstRow = showTop ? 0 : store.Height() - visibleRows;
    image.width = std::max(1, int(store.Width() * ratio));
    image.height = std::max(1, std::min(maxHeight, int(visibleRows * ratio)));
    image.rgba.resize(size_t(image.width) * image.height * 4);
    std::vector<uint8_t> row(size_t(store.Width()) * 4);
    for (int y = 0; y < image.height; ++y) {
        store.ReadRows(firstRow + int(int64_t(y) * visibleRows / image.height), 1, row.data(), store.Width() * 4);
        for (int x = 0; x < image.width; ++x) {
            std::memcpy(image.rgba.data() + (size_t(y) * image.width + x) * 4,
                        row.data() + (int64_t(x) * store.Width() / image.width) * 4, 4);
        }
    }
    return image;
}

ScrollCaptureController::~ScrollCaptureController() {
    Cancel();
    if (m_worker.joinable()) m_worker.join();
}

bool ScrollCaptureController::Start(std::unique_ptr<ScrollCaptureBackend> backend,
                                     const StitcherConfig& limits) {
    {
        std::lock_guard lock(m_mutex);
        if (!m_exited || !backend) return false;
    }
    if (m_worker.joinable()) m_worker.join();
    uint64_t epoch;
    {
        std::lock_guard lock(m_mutex);
        epoch = ++m_epoch;
        m_cancel = m_finish = false;
        m_exited = false;
        m_pending = 0;
        m_stepInFlight = false;
        m_direction = 0;
        m_snapshot = {};
        m_snapshot.epoch = epoch;
        m_snapshot.state = ScrollCaptureState::Preparing;
        m_snapshot.message = Tr(m_language, TextId::ScrollPreparing);
        ++m_snapshot.revision;
    }
    m_worker = std::thread([this, epoch, backend = std::move(backend), limits]() mutable {
        try { Run(epoch, std::move(backend), limits); }
        catch (const std::exception& e) {
            auto snapshot = Snapshot();
            snapshot.state = ScrollCaptureState::Failed;
            Logger::Error("[ScrollCapture] worker failed: {}", e.what());
            snapshot.message = Tr(m_language, TextId::ScrollFailed);
            Publish(epoch, std::move(snapshot));
        }
        std::lock_guard lock(m_mutex);
        m_exited = true;
    });
    return true;
}

bool ScrollCaptureController::IsStopped() const {
    std::lock_guard lock(m_mutex);
    return m_exited;
}
void ScrollCaptureController::Join() {
    if (m_worker.joinable()) m_worker.join();
}

bool ScrollCaptureController::Current(uint64_t epoch) const {
    std::lock_guard lock(m_mutex);
    return epoch == m_epoch && !m_cancel;
}
void ScrollCaptureController::Publish(uint64_t epoch, ScrollCaptureSnapshot snapshot) {
    std::lock_guard lock(m_mutex);
    PublishLocked(epoch, std::move(snapshot));
}
void ScrollCaptureController::PublishLocked(uint64_t epoch, ScrollCaptureSnapshot snapshot) {
    if (epoch != m_epoch || m_cancel) return;
    // 输入提示由 AddIntent 更新，后台迟到的采帧结果不能覆盖它。
    snapshot.reverseScrollBlocked = m_snapshot.reverseScrollBlocked;
    if (!snapshot.preview) snapshot.preview = m_snapshot.preview;
    snapshot.epoch = epoch;
    snapshot.revision = m_snapshot.revision + 1;
    if (m_finish && (snapshot.state == ScrollCaptureState::Capturing || snapshot.state == ScrollCaptureState::Preparing)) snapshot.state = ScrollCaptureState::Finalizing;
    m_snapshot = std::move(snapshot);
}
void ScrollCaptureController::CompleteStep(uint64_t epoch, ScrollCaptureBackend& backend,
                                          ScrollCaptureSnapshot snapshot) {
    // 清理可能回调 AddIntent 更新方向提示，必须在控制器锁外完成。
    backend.DiscardPendingInput();
    std::lock_guard lock(m_mutex);
    // 最终快照一旦可见就必须允许下一步，避免观察到 Failed 后立即重试却被丢弃。
    PublishLocked(epoch, std::move(snapshot));
    m_stepInFlight = false;
}
void ScrollCaptureController::PublishPreview(uint64_t epoch, uint64_t version,
                                             std::shared_ptr<const ScrollImage> preview) {
    std::lock_guard lock(m_mutex);
    if (epoch != m_epoch || m_cancel || version != m_snapshot.imageVersion) return;
    m_snapshot.preview = std::move(preview);
    ++m_snapshot.revision;
}
ScrollCaptureSnapshot ScrollCaptureController::Snapshot() const {
    std::lock_guard lock(m_mutex);
    return m_snapshot;
}
void ScrollCaptureController::AddIntent(uint64_t epoch, int64_t delta) {
    std::lock_guard lock(m_mutex);
    // 失败时接受触发，但工作线程只原地重取，不能继续滚动破坏重叠。
    if (epoch != m_epoch || m_cancel || m_finish || delta == 0 ||
        (m_snapshot.state != ScrollCaptureState::Capturing &&
         m_snapshot.state != ScrollCaptureState::Failed)) return;
    const int direction = delta > 0 ? 1 : -1;
    if (m_direction != 0 && direction != m_direction) {
        if (!m_snapshot.reverseScrollBlocked) {
            m_snapshot.reverseScrollBlocked = true;
            ++m_snapshot.revision;
        }
        return;
    }
    if (m_snapshot.reverseScrollBlocked) {
        m_snapshot.reverseScrollBlocked = false;
        ++m_snapshot.revision;
    }
    m_direction = direction;
    if (m_stepInFlight) return;
    m_pending = direction * m_stepPixels;
    m_wake.notify_all();
}
void ScrollCaptureController::Finish() {
    std::lock_guard lock(m_mutex);
    if (m_cancel || m_finish || m_exited) return;
    m_finish = true;
    if (m_backend) m_backend->PauseInput();
    m_pending = 0;
    m_finishStarted = std::chrono::steady_clock::now();
    m_finishDeadline = m_finishStarted + std::chrono::milliseconds(500);
    Logger::Info("event=scroll.confirm_requested");
    m_snapshot.state = ScrollCaptureState::Finalizing;
    m_snapshot.message = Tr(m_language, TextId::ScrollFinalizing);
    ++m_snapshot.revision;
    m_wake.notify_all();
}
void ScrollCaptureController::Cancel() {
    std::lock_guard lock(m_mutex);
    if (m_cancel) return;
    m_cancel = true;
    if (m_backend) m_backend->RequestCancel();
    ++m_epoch; // 立即使后台中的旧会话失效，迟到结果不能回填。
    m_pending = 0;
    m_snapshot = {};
    m_snapshot.epoch = m_epoch;
    m_snapshot.state = ScrollCaptureState::Cancelled;
    ++m_snapshot.revision;
    m_wake.notify_all();
}

void ScrollCaptureController::RunContinuous(uint64_t epoch, ScrollCaptureBackend& backend,
    ScrollCaptureStitcher& stitcher, ScrollCaptureSnapshot& state, const StitcherConfig& limits,
    const std::function<void()>& schedulePreview) {
    backend.ConfigureContinuousInput(state.viewport->height, limits.minOverlapRows);
    bool limitReached = false;
    std::optional<std::chrono::steady_clock::time_point> quietSince;
    const auto observeQuiet = [&quietSince](bool quiet) {
        if (!quiet) quietSince.reset();
        else if (!quietSince) quietSince = std::chrono::steady_clock::now();
    };
    while (Current(epoch)) {
        {
            std::lock_guard lock(m_mutex);
            m_pending = 0; // 连续模式的输入距离由平台调度器消费。
            const auto now = std::chrono::steady_clock::now();
            if (m_finish && (now >= m_finishDeadline ||
                (quietSince && now - *quietSince >= std::chrono::milliseconds(80) &&
                 backend.FinalFrameSettled()))) break;
        }
        if (backend.CancelRequested()) { Cancel(); return; }
        std::string error;
        if (!backend.ValidateTarget(error)) {
            observeQuiet(false);
            backend.FrameFeedback({StitchStatus::MatchFailed}, false);
            if (state.state != ScrollCaptureState::Failed) {
                state.state = ScrollCaptureState::Failed;
                state.message = FormatText(m_language, TextId::ScrollKeepResult, error);
                Publish(epoch, state);
            }
            std::unique_lock lock(m_mutex);
            m_wake.wait_for(lock, std::chrono::milliseconds(30));
            continue;
        }
        ScrollImage image;
        bool stable = false;
        const auto start = std::chrono::steady_clock::now();
        if (!backend.CaptureStep(0, image, stable, error)) {
            if (!error.empty()) {
                observeQuiet(false);
                backend.FrameFeedback({StitchStatus::MatchFailed}, false);
                state.state = ScrollCaptureState::Failed;
                state.message = FormatText(m_language, TextId::ScrollKeepResult, error);
                Publish(epoch, state);
                std::unique_lock lock(m_mutex);
                m_wake.wait_for(lock, std::chrono::milliseconds(30));
            }
            continue; // 有界取帧超时不等于失败；静止画面可能没有新样本。
        }
        if (!Current(epoch)) return;
        if (!backend.ValidateTarget(error)) { observeQuiet(false); continue; }
        if (limitReached) { observeQuiet(false); continue; }
        int direction;
        { std::lock_guard lock(m_mutex); direction = m_direction; }
        // 尚未滚动时不把页面动画当成滚动；只有可靠位移才更新基准和长图。
        if (direction == 0) {
            observeQuiet(IsFrameStable(image.View(), state.viewport->View()));
            continue;
        }
        const int contentHeight = image.height - stitcher.FixedHeaderHeight() - stitcher.FixedFooterHeight();
        const auto matchStart = std::chrono::steady_clock::now();
        StitchResult result;
        if (image.sharedRgba && image.sharedRgba == state.viewport->sharedRgba &&
            image.width == state.viewport->width && image.height == state.viewport->height) {
            // Idle 复用已接受像素，无需重复计算签名和验证整帧。
            result.status = StitchStatus::NoChange;
            result.confidence = 1.0f;
        } else {
            result = stitcher.ProcessFrame({image.View(), 0, false, stable, direction,
                                           std::max(1, contentHeight / 2)});
        }
        const bool matched = result.status == StitchStatus::Ok || result.status == StitchStatus::NoChange;
        observeQuiet(result.status == StitchStatus::NoChange);
        backend.FrameFeedback(result, matched,
            image.height - stitcher.FixedHeaderHeight() - stitcher.FixedFooterHeight());
        Logger::Debug("[ScrollCapture] continuous status={} dy={} confidence={:.3f} anchor={} columnExclusion={} captureMs={:.2f} matchMs={:.2f}",
            int(result.status), result.dy, result.confidence, result.usedAnchorSearch, result.usedColumnExclusion,
            std::chrono::duration<double, std::milli>(matchStart - start).count(),
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - matchStart).count());
        if (!matched) {
            limitReached = result.status == StitchStatus::LimitReached;
            state.state = ScrollCaptureState::Failed;
            state.message = FormatText(m_language, TextId::ScrollRetryOrFinish,
                limitReached ? Tr(m_language, TextId::ScrollLimitReached)
                             : Tr(m_language, stitcher.GetLastErrorId()));
            Publish(epoch, state);
            continue;
        }
        const bool recovered = state.state == ScrollCaptureState::Failed;
        state.state = ScrollCaptureState::Capturing;
        if (result.status == StitchStatus::NoChange) {
            if (recovered) { state.message = Tr(m_language, TextId::ScrollCapturing); Publish(epoch, state); }
            continue; // 单次零位移不代表到底，也不重复刷新预览。
        }
        state.viewport = std::make_shared<ScrollImage>(std::move(image));
        ++state.imageVersion;
        if (result.appendedTopRows > 0) state.expandedTop = true;
        else if (result.appendedBottomRows > 0) state.expandedTop = false;
        state.totalHeight = stitcher.GetStore()->Height() + stitcher.FixedHeaderHeight() + stitcher.FixedFooterHeight();
        state.message = Tr(m_language, TextId::ScrollCapturing);
        Publish(epoch, state);
        schedulePreview();
    }
}

void ScrollCaptureController::Run(uint64_t epoch, std::unique_ptr<ScrollCaptureBackend> backend,
                                   StitcherConfig limits) {
    {
        std::lock_guard lock(m_mutex);
        if (m_cancel || epoch != m_epoch) return;
        m_backend = backend.get();
    }
    struct StopGuard {
        ScrollCaptureController* owner;
        ScrollCaptureBackend* backend;
        ~StopGuard() {
            backend->Stop();
            std::lock_guard lock(owner->m_mutex);
            owner->m_backend = nullptr;
        }
    } guard{this, backend.get()};
    ScrollCaptureSnapshot state;
    std::string error;
    ScrollImage first;
    if (!backend->Start(first, [this, epoch](int64_t d) { AddIntent(epoch, d); }, error)) {
        state.state = ScrollCaptureState::Failed;
        state.message = error;
        Publish(epoch, state);
        return;
    }
    if (!Current(epoch)) return;
    ScrollCaptureStitcher stitcher(limits);
    if (!stitcher.StartSession(first.View())) {
        state.state = ScrollCaptureState::Failed;
        state.message = Tr(m_language, stitcher.GetLastErrorId());
        Publish(epoch, state);
        return;
    }
    {
        std::lock_guard lock(m_mutex);
        // 固定步长取视口的 25%；小选区预留最小重叠，并容忍应用实际滚动量放大。
        m_stepPixels = std::max<int64_t>(1, std::min(first.height / 4,
                                                    (first.height - limits.minOverlapRows) / 2));
    }
    state.viewport = std::make_shared<ScrollImage>(std::move(first));
    state.totalHeight = state.viewport->height;
    state.state = ScrollCaptureState::Capturing;
    state.message = Tr(m_language, TextId::ScrollReady);
    Publish(epoch, state);
    PreviewWorker previews([this, epoch](uint64_t version, std::shared_ptr<const ScrollImage> image) {
        PublishPreview(epoch, version, std::move(image));
    });
    const auto schedulePreview = [&] {
        if (stitcher.GetStore()->Height() > 0)
            previews.Submit(*stitcher.GetStore(), state.imageVersion, state.expandedTop);
    };
    const auto clearPending = [this]() {
        std::lock_guard lock(m_mutex);
        m_pending = 0;
    };
    bool retryInPlace = false;
    int expectedShift = 0;
    int blankAdvanceRetries = 0;
    bool blankRecoveryStopped = false;
    if (backend->ContinuousInput()) {
        RunContinuous(epoch, *backend, stitcher, state, limits, schedulePreview);
    } else while (Current(epoch)) {
        int64_t pending;
        {
            std::unique_lock lock(m_mutex);
            if (m_finish) break;
            if (m_pending == 0) m_wake.wait_for(lock, std::chrono::milliseconds(50));
            pending = std::exchange(m_pending, 0);
            if (m_finish || m_cancel) break;
            if (pending != 0) m_stepInFlight = true;
        }
        // 提前退出和异常仍需清理输入；正常完成由 CompleteStep 同步发布结果并开放下一步。
        struct StepGuard {
            ScrollCaptureController* owner;
            ScrollCaptureBackend* backend;
            bool active;
            ~StepGuard() {
                if (!active) return;
                // tap 输入可能尚未转发到 Controller，也要清空这一层暂存。
                backend->DiscardPendingInput();
                std::lock_guard lock(owner->m_mutex);
                owner->m_stepInFlight = false;
            }
        } stepGuard{this, backend.get(), pending != 0};
        const auto publishCompletedStep = [&] {
            CompleteStep(epoch, *backend, state);
            // 已完成清理，作用域退出时不能再次清空刚接受的重试输入。
            stepGuard.active = false;
        };
        if (backend->CancelRequested()) { Cancel(); return; }
        if (pending == 0) {
            const bool valid = backend->ValidateTarget(error); // 失败暂停期间也要接收重试触发。
            if (!valid && state.state != ScrollCaptureState::Failed) {
                state.state = ScrollCaptureState::Failed;
                state.message = FormatText(m_language, TextId::ScrollKeepResult, error);
                clearPending();
                Publish(epoch, state);
            }
            continue;
        }
        if (!backend->ValidateTarget(error)) {
            state.state = ScrollCaptureState::Failed;
            state.message = FormatText(m_language, TextId::ScrollKeepResult, error);
            clearPending();
            publishCompletedStep();
            continue;
        }
        {
            std::lock_guard lock(m_mutex);
            if (m_finish || m_cancel) break;
        }
        const int64_t inject = retryInPlace ? 0 : pending;
        if (inject != 0) expectedShift = int(inject);
        Logger::Debug("[WinScrollCapture][controller] step pending={} inject={} expectedShift={} retryInPlace={}",
                     pending, inject, expectedShift, retryInPlace);
        // 注入后优先在当前位置恢复；仅有效内容不足时允许受预算限制的追加滚动。
        retryInPlace = true;
        ScrollImage image;
        bool stable = false;
        bool captured = backend->CaptureStep(inject, image, stable, error);
        if (!Current(epoch)) return;
        {
            std::lock_guard lock(m_mutex);
            if (m_finish && std::chrono::steady_clock::now() > m_finishDeadline) break;
        }
        if (captured && !backend->ValidateTarget(error)) captured = false;
        if (!captured) {
            state.state = ScrollCaptureState::Failed;
            state.message = FormatText(m_language, TextId::ScrollKeepResult, error);
            clearPending();
            publishCompletedStep();
            continue;
        }
        const int direction = expectedShift > 0 ? 1 : -1;
        std::string matchError;
        const auto processFrame = [&]() {
            if (!stable) {
                matchError = Tr(m_language, TextId::FrameUnstable);
                StitchResult unsettled;
                unsettled.status = StitchStatus::MatchFailed;
                return unsettled;
            }
            const auto result = stitcher.ProcessFrame({image.View(), expectedShift, true, true, direction});
            matchError = result.status == StitchStatus::Ok || result.status == StitchStatus::NoChange
                ? std::string() : Tr(m_language, stitcher.GetLastErrorId());
            return result;
        };
        auto result = processFrame();
        Logger::Debug("[WinScrollCapture][stitch] first status={} dy={} confidence={:.3f} appendedTop={} appendedBottom={} fallback={} anchor={} stable={} reason={}",
                     int(result.status), result.dy, result.confidence,
                     result.appendedTopRows, result.appendedBottomRows,
                     result.usedFallbackSearch, result.usedAnchorSearch, stable, matchError);
        if (result.status == StitchStatus::MatchFailed || result.status == StitchStatus::Ambiguous ||
            result.status == StitchStatus::InsufficientInfo) {
            Logger::Debug("[ScrollCapture] stitch failed: status={}, reason={}, expectedShift={}, confidence={:.2f}, stable={}",
                            int(result.status), matchError, expectedShift,
                            result.confidence, stable);
            bool finishing;
            { std::lock_guard lock(m_mutex); finishing = m_finish; }
            if (!finishing) {
                // 同一步重新取帧，恢复失败时停止，不能继续滚动破坏最后成功基准。
                captured = backend->CaptureStep(0, image, stable, error);
                if (!Current(epoch)) return;
                {
                    std::lock_guard lock(m_mutex);
                    if (m_finish && std::chrono::steady_clock::now() > m_finishDeadline) break;
                }
                if (captured && !backend->ValidateTarget(error)) captured = false;
                if (captured) {
                    result = processFrame();
                    Logger::Debug("[WinScrollCapture][stitch] retry status={} dy={} confidence={:.3f} appendedTop={} appendedBottom={} fallback={} anchor={} stable={} reason={}",
                                 int(result.status), result.dy, result.confidence,
                                 result.appendedTopRows, result.appendedBottomRows,
                                 result.usedFallbackSearch, result.usedAnchorSearch, stable, matchError);
                }

            }
        }
        // 先原地重取以排除暂时加载；仍然缺少特征时才有限度地继续滚动。
        // 失败帧不会提交，因此先验必须累计到最后一张成功帧，而非仅使用本次注入量。
        bool recoveryInterrupted = false;
        int64_t recoveryStep = std::max<int64_t>(1, std::abs(expectedShift));
        while (captured && result.status == StitchStatus::InsufficientInfo &&
               !blankRecoveryStopped && blankAdvanceRetries < 2) {
            {
                std::lock_guard lock(m_mutex);
                if (m_finish || m_cancel) break;
            }
            if (!Current(epoch)) return;
            if (backend->CancelRequested()) { Cancel(); return; }
            if (!backend->ValidateTarget(error)) { captured = false; break; }
            {
                std::lock_guard lock(m_mutex);
                if (m_finish || m_cancel) break;
            }
            const int64_t usableHeight = image.height - stitcher.FixedHeaderHeight() - stitcher.FixedFooterHeight();
            const int64_t remaining = usableHeight - limits.minOverlapRows - std::abs(expectedShift);
            if (remaining <= 0) { blankRecoveryStopped = true; break; }
            recoveryStep = std::min(remaining, recoveryStep + std::max<int64_t>(1, recoveryStep / 2));
            const int64_t extra = direction * recoveryStep;
            expectedShift += static_cast<int>(extra);
            ++blankAdvanceRetries;
            state.message = FormatText(m_language, TextId::ScrollRetryCount, blankAdvanceRetries);
            Publish(epoch, state);
            captured = backend->CaptureStep(extra, image, stable, error);
            if (!Current(epoch)) return;
            {
                std::lock_guard lock(m_mutex);
                if (m_finish) { recoveryInterrupted = true; break; }
            }
            if (captured && !backend->ValidateTarget(error)) captured = false;
            {
                std::lock_guard lock(m_mutex);
                if (m_finish) { recoveryInterrupted = true; break; }
            }
            if (!captured) break;
            result = processFrame();
            Logger::Debug("[ScrollCapture] blank recovery attempt={} inject={} expectedShift={} status={} confidence={:.3f}",
                         blankAdvanceRetries, extra, expectedShift, int(result.status), result.confidence);
        }
        {
            std::lock_guard lock(m_mutex);
            if (m_finish && recoveryInterrupted) break;
        }
        // 自动推进失败后锁定原地重试，避免用户再次触发时重置预算并越过重叠区。
        if (blankAdvanceRetries > 0 || result.status == StitchStatus::InsufficientInfo) {
            blankRecoveryStopped = true;
        }
        const bool matched = captured && (result.status == StitchStatus::Ok || result.status == StitchStatus::NoChange);
        if (!matched) {
            state.state = ScrollCaptureState::Failed;
            // 透出拼接器内部的具体失败原因（置信度不足/位移歧义/空白画面等），
            // 便于区分偶发的动画干扰与系统性阈值问题
            const std::string reason = result.status == StitchStatus::LimitReached
                ? Tr(m_language, TextId::ScrollLimitReached)
                : FormatText(m_language, TextId::ScrollMatchFailed, captured ? matchError : error);
            state.message = FormatText(m_language, TextId::ScrollRetryOrFinish, reason);
            Logger::Warning("[ScrollCapture] step failed after retry: status={}, reason={}, dy={}, confidence={:.2f}",
                            int(result.status), captured ? stitcher.GetLastErrorMessage() : error, result.dy, result.confidence);
            clearPending();
            if (inject == 0 && !blankRecoveryStopped) {
                // 原地重试步本身失败：复位 in-place 恢复，让用户的下一次滚动重新驱动注入。
                // 否则 retryInPlace 永不复位，inject 被锁死为 0，失败后用户滚动永远无效（死锁）。
                retryInPlace = false;
            }
        } else {
            retryInPlace = false;
            blankAdvanceRetries = 0;
            blankRecoveryStopped = false;
            // 失败后用户滚动重试成功：恢复采集状态（Failed 只在 !matched 分支设置）
            if (state.state == ScrollCaptureState::Failed) {
                state.state = ScrollCaptureState::Capturing;
            }
            state.viewport = std::make_shared<ScrollImage>(std::move(image));
            ++state.imageVersion;
            if (stitcher.GetStore()->Height() > 0) {
                if (result.appendedTopRows > 0) state.expandedTop = true;
                else if (result.appendedBottomRows > 0) state.expandedTop = false;
                state.totalHeight = stitcher.GetStore()->Height() + stitcher.FixedHeaderHeight() + stitcher.FixedFooterHeight();
            }
            if (result.status == StitchStatus::NoChange) {
                clearPending();
                state.message = Tr(m_language, TextId::ScrollBoundary);
            } else state.message = Tr(m_language, TextId::ScrollCapturing);
        }
        publishCompletedStep();
        if (matched) schedulePreview();
    }
    if (!Current(epoch)) return;
    const auto drained = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point confirmed;
    { std::lock_guard lock(m_mutex); confirmed = m_finishStarted; }
    backend->Stop();
    const auto stopped = std::chrono::steady_clock::now();
    auto result = std::make_shared<ScrollImage>();
    if (!stitcher.ExportFinalImage(result->rgba, result->width, result->height)) {
        state.state = ScrollCaptureState::Failed;
        state.message = Tr(m_language, stitcher.GetLastErrorId());
    } else {
        state.state = ScrollCaptureState::Done;
        state.result = std::move(result);
        state.message = Tr(m_language, TextId::ScrollComplete);
    }
    Logger::Info("event=scroll.confirm_finished drain_ms={:.1f} stop_ms={:.1f} export_ms={:.1f} total_ms={:.1f}",
        std::chrono::duration<double, std::milli>(drained - confirmed).count(),
        std::chrono::duration<double, std::milli>(stopped - drained).count(),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - stopped).count(),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - confirmed).count());
    Publish(epoch, state);
}
} // namespace shotlet
