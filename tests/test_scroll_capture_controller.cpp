#include <gtest/gtest.h>
#include "scrollcapture/ScrollCaptureController.h"
#include "scrollcapture/ScrollCaptureTypes.h"
#include "scrollcapture/ScrollTarget.h"
#include "scrollcapture/ScrollPointerGuard.h"
#include "output/LongImageLayout.h"
#include "utils/ScopeExit.h"
#include "scroll_capture_test_data.h"
#include <future>
#include <deque>
#include <thread>
#include <utility>

using namespace shotlet;
namespace {
struct FakeState {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<int64_t> injections;
    bool blocked = false, entered = false, release = false;
    bool targetValid = true, failCapture = false, badImage = false;
    bool blankImage = false, reverseMovement = false;
    int64_t polledIntent = 0;
    bool stableImage = true;
    int blockOnCapture = 0;
    int blankImageCount = 0;
    int badImageCount = 0;  // 前 N 次取帧反转像素（N=2 时首帧+重试都失败，走 Failed）
    std::atomic<int> stops{0};
    int y = 200;
    std::vector<uint8_t> page = scrolltest::MakeTexturedLongImage(200, 2000, 7);
};
class FakeBackend : public ScrollCaptureBackend {
public:
    explicit FakeBackend(std::shared_ptr<FakeState> state) : s(std::move(state)) {}
    bool Start(ScrollImage& first, std::function<void(int64_t)> callback, std::string&) override {
        m_callback = std::move(callback);
        first = Frame(); return true;
    }
    bool CaptureStep(int64_t delta, ScrollImage& image, bool& stable, std::string& error) override {
        std::unique_lock lock(s->mutex);
        s->injections.push_back(delta);
        s->entered = true;
        s->cv.notify_all();
        if (s->blocked || s->blockOnCapture == int(s->injections.size())) s->cv.wait(lock, [&] { return s->release; });
        if (s->failCapture) { error = "模拟采集失败"; return false; }
        s->y = std::clamp(s->y + int(s->reverseMovement ? -delta : delta), 0, 1400);
        image = Frame();
        // 反转像素构造"非空白但无法匹配"的帧：走 MatchFailed 路径
        // （纯白帧会命中 InsufficientInfo，与测试意图不符）
        if (s->badImage) for (auto& b : image.rgba) b = static_cast<uint8_t>(255 - b);
        // 仅前 N 次失败：验证 Failed 后用户滚动可恢复采集
        if (s->badImageCount > 0) {
            --s->badImageCount;
            for (auto& b : image.rgba) b = static_cast<uint8_t>(255 - b);
        }
        // 纯白帧：空白画面走 InsufficientInfo（大面积留白，无判别力行）
        if (s->blankImage || s->blankImageCount > 0) {
            if (s->blankImageCount > 0) --s->blankImageCount;
            std::fill(image.rgba.begin(), image.rgba.end(), 255);
        }
        stable = s->stableImage;
        return true;
    }
    bool ValidateTarget(std::string& error) override {
        std::lock_guard lock(s->mutex);
        if (s->polledIntent != 0) {
            const auto delta = s->polledIntent;
            s->polledIntent = 0;
            m_callback(delta);
        }
        if (!s->targetValid) { error = "目标发生变化"; return false; }
        return true;
    }
    void Stop() override { if (!stopped) { ++s->stops; stopped = true; } }
private:
    ScrollImage Frame() {
        scrolltest::FrameSpec spec;
        spec.contentY = s->y; spec.viewportHeight = 500;
        return {200, 500, scrolltest::CutFrame(s->page, 200, 2000, spec)};
    }
    std::function<void(int64_t)> m_callback;
    std::shared_ptr<FakeState> s;
    bool stopped = false;
};

constexpr auto ControllerWaitTimeout = std::chrono::seconds(3);
constexpr auto ControllerPollInterval = std::chrono::milliseconds(2);
constexpr int FailedStepCaptureCount = 2;

struct StepCleanupGate {
    std::promise<void> entered;
    std::promise<void> resume;
    std::shared_future<void> resumed = resume.get_future().share();
    std::atomic<bool> released{false};

    void Release() {
        if (!released.exchange(true)) resume.set_value();
    }
};

class CleanupGatedBackend final : public FakeBackend {
public:
    CleanupGatedBackend(std::shared_ptr<FakeState> state, std::shared_ptr<StepCleanupGate> gate)
        : FakeBackend(std::move(state)), m_gate(std::move(gate)) {}

    void DiscardPendingInput() override {
        if (!std::exchange(m_firstCleanup, false)) return;
        m_gate->entered.set_value();
        m_gate->resumed.wait();
    }

private:
    std::shared_ptr<StepCleanupGate> m_gate;
    bool m_firstCleanup = true;
};

bool WaitState(ScrollCaptureController& controller, ScrollCaptureState desired) {
    auto until = std::chrono::steady_clock::now() + ControllerWaitTimeout;
    while (std::chrono::steady_clock::now() < until) {
        if (controller.Snapshot().state == desired) return true;
        std::this_thread::sleep_for(ControllerPollInterval);
    }
    return false;
}
bool WaitEntered(const std::shared_ptr<FakeState>& state) {
    std::unique_lock lock(state->mutex);
    return state->cv.wait_for(lock, std::chrono::seconds(2), [&] { return state->entered; });
}
void Release(const std::shared_ptr<FakeState>& state) {
    std::lock_guard lock(state->mutex);
    state->release = true; state->cv.notify_all();
}

struct ContinuousState {
    struct Frame { int y; bool corrupt = false; };
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Frame> frames;
    std::vector<StitchResult> feedback;
    std::atomic<bool> paused{false}, cancelled{false};
    std::atomic<bool> settled{false};
    std::atomic<int> injectedSteps{0}, stops{0};
    const std::vector<uint8_t> page = scrolltest::MakeTexturedLongImage(200, 2000, 7);
    void Push(int y, bool corrupt = false) {
        std::lock_guard lock(mutex); frames.push_back({y, corrupt}); cv.notify_all();
    }
    bool WaitFeedback(size_t count) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, std::chrono::seconds(3), [&] { return feedback.size() >= count; });
    }
};
class ContinuousBackend : public ScrollCaptureBackend {
public:
    explicit ContinuousBackend(std::shared_ptr<ContinuousState> s) : s(std::move(s)) {}
    bool Start(ScrollImage& image, std::function<void(int64_t)>, std::string&) override {
        image = Frame(200); return true;
    }
    bool ContinuousInput() const override { return true; }
    bool CaptureStep(int64_t delta, ScrollImage& image, bool& stable, std::string&) override {
        if (delta) ++s->injectedSteps;
        std::unique_lock lock(s->mutex);
        s->cv.wait_for(lock, std::chrono::milliseconds(10), [&] { return !s->frames.empty() || s->cancelled; });
        if (s->frames.empty()) return false;
        const auto frame = s->frames.front(); s->frames.pop_front();
        image = Frame(frame.y);
        if (frame.corrupt) for (auto& pixel : image.rgba) pixel = uint8_t(255 - pixel);
        stable = false;
        return true;
    }
    bool ValidateTarget(std::string&) override { return true; }
    void FrameFeedback(const StitchResult& result, bool, int) override {
        std::lock_guard lock(s->mutex); s->feedback.push_back(result); s->cv.notify_all();
    }
    void PauseInput() override { s->paused = true; }
    bool FinalFrameSettled() const override { return s->paused && s->settled; }
    void RequestCancel() override { PauseInput(); s->cancelled = true; s->cv.notify_all(); }
    void Stop() override { PauseInput(); if (!stopped) { ++s->stops; stopped = true; } }
private:
    ScrollImage Frame(int y) {
        scrolltest::FrameSpec spec; spec.contentY = y; spec.viewportHeight = 500;
        return {200, 500, scrolltest::CutFrame(s->page, 200, 2000, spec)};
    }
    std::shared_ptr<ContinuousState> s;
    bool stopped = false;
};
}

TEST(ScrollCaptureControllerTest, ContinuousFramesMatchDuringMotionAndRecoverFromDiscardedFrame) {
    auto backend = std::make_shared<ContinuousState>();
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<ContinuousBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.AddIntent(controller.Snapshot().epoch, 1);
    backend->Push(215);
    ASSERT_TRUE(backend->WaitFeedback(1));
    backend->Push(230);
    ASSERT_TRUE(backend->WaitFeedback(2));
    backend->Push(245, true);
    ASSERT_TRUE(backend->WaitFeedback(3));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Failed));
    EXPECT_EQ(controller.Snapshot().totalHeight, 530);
    backend->Push(250);
    ASSERT_TRUE(backend->WaitFeedback(4));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.Finish();
    EXPECT_TRUE(backend->paused);
    controller.Join();
    const auto result = controller.Snapshot();
    ASSERT_EQ(result.state, ScrollCaptureState::Done);
    ASSERT_NE(result.result, nullptr);
    EXPECT_EQ(result.result->height, 550);
    scrolltest::FrameSpec spec; spec.contentY = 200; spec.viewportHeight = 550;
    EXPECT_EQ(result.result->rgba, scrolltest::CutFrame(backend->page, 200, 2000, spec));
    ASSERT_NE(result.preview, nullptr); // Join 同时等待最后一版异步预览。
    LongImageStore store(200, 1024);
    ASSERT_EQ(store.Initialize(result.result->View()), AppendResult::Ok);
    EXPECT_EQ(result.preview->rgba, MakeScrollPreview(store).rgba);
    EXPECT_EQ(backend->injectedSteps, 0);
    EXPECT_EQ(backend->stops, 1);
    EXPECT_EQ(backend->feedback[0].dy, 15);
    EXPECT_EQ(backend->feedback[1].dy, 15);
    EXPECT_EQ(backend->feedback[3].dy, 20); // 失败帧未替换成功基准。
}

TEST(ScrollCaptureControllerTest, ContinuousNoChangeDoesNotFinishAndFinalizationDrainsDelayedMotion) {
    auto backend = std::make_shared<ContinuousState>();
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<ContinuousBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.AddIntent(controller.Snapshot().epoch, 1);
    backend->Push(200);
    ASSERT_TRUE(backend->WaitFeedback(1));
    EXPECT_EQ(controller.Snapshot().state, ScrollCaptureState::Capturing);
    controller.Finish();
    EXPECT_TRUE(backend->paused);
    backend->Push(220); // 已停止注入，但目标应用的最后一次动画尚未完成。
    controller.Join();
    ASSERT_EQ(controller.Snapshot().state, ScrollCaptureState::Done);
    ASSERT_NE(controller.Snapshot().result, nullptr);
    EXPECT_EQ(controller.Snapshot().result->height, 520);
}

TEST(ScrollCaptureControllerTest, AlreadyQuietContinuousCaptureFinishesBeforeDeadline) {
    auto backend = std::make_shared<ContinuousState>();
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<ContinuousBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.AddIntent(controller.Snapshot().epoch, 1);
    backend->Push(200);
    ASSERT_TRUE(backend->WaitFeedback(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    backend->settled = true;
    controller.Finish();
    auto completed = std::async(std::launch::async, [&] { controller.Join(); });
    EXPECT_EQ(completed.wait_for(std::chrono::milliseconds(300)), std::future_status::ready);
    completed.get();
    ASSERT_EQ(controller.Snapshot().state, ScrollCaptureState::Done);
    EXPECT_EQ(controller.Snapshot().result->height, 500);
    EXPECT_EQ(backend->stops, 1);
}

TEST(ScrollCaptureControllerTest, EarlyFinishKeepsDelayedTailAndWaitsForQuiet) {
    auto backend = std::make_shared<ContinuousState>();
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<ContinuousBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.AddIntent(controller.Snapshot().epoch, 1);
    backend->Push(200);
    ASSERT_TRUE(backend->WaitFeedback(1));
    controller.Finish();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    EXPECT_EQ(controller.Snapshot().state, ScrollCaptureState::Finalizing);
    backend->Push(220);
    ASSERT_TRUE(backend->WaitFeedback(2));
    backend->Push(220);
    ASSERT_TRUE(backend->WaitFeedback(3));
    backend->settled = true;
    auto completed = std::async(std::launch::async, [&] { controller.Join(); });
    EXPECT_EQ(completed.wait_for(std::chrono::milliseconds(300)), std::future_status::ready);
    completed.get();
    const auto result = controller.Snapshot();
    ASSERT_EQ(result.state, ScrollCaptureState::Done);
    ASSERT_NE(result.result, nullptr);
    EXPECT_EQ(result.result->height, 520);
    scrolltest::FrameSpec spec; spec.contentY = 200; spec.viewportHeight = 520;
    EXPECT_EQ(result.result->rgba, scrolltest::CutFrame(backend->page, 200, 2000, spec));
}

TEST(ScrollCaptureControllerTest, MotionInvalidatesPreviouslyQuietFinish) {
    auto backend = std::make_shared<ContinuousState>();
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<ContinuousBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.AddIntent(controller.Snapshot().epoch, 1);
    backend->Push(200);
    ASSERT_TRUE(backend->WaitFeedback(1));
    std::this_thread::sleep_for(std::chrono::milliseconds(90));
    backend->Push(220);
    ASSERT_TRUE(backend->WaitFeedback(2));
    backend->settled = true;
    controller.Finish();
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(controller.Snapshot().state, ScrollCaptureState::Finalizing);
    backend->Push(240);
    ASSERT_TRUE(backend->WaitFeedback(3));
    backend->Push(240);
    ASSERT_TRUE(backend->WaitFeedback(4));
    auto completed = std::async(std::launch::async, [&] { controller.Join(); });
    EXPECT_EQ(completed.wait_for(std::chrono::milliseconds(300)), std::future_status::ready);
    completed.get();
    ASSERT_EQ(controller.Snapshot().state, ScrollCaptureState::Done);
    ASSERT_NE(controller.Snapshot().result, nullptr);
    EXPECT_EQ(controller.Snapshot().result->height, 540);
}

TEST(ScrollCaptureControllerTest, CancelledContinuousSessionCannotPublishLateFramesOrPreview) {
    auto backend = std::make_shared<ContinuousState>();
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<ContinuousBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.AddIntent(controller.Snapshot().epoch, 1);
    backend->Push(220);
    ASSERT_TRUE(backend->WaitFeedback(1));
    controller.Cancel();
    backend->Push(240);
    controller.Join();
    const auto cancelled = controller.Snapshot();
    EXPECT_EQ(cancelled.state, ScrollCaptureState::Cancelled);
    EXPECT_EQ(cancelled.preview, nullptr);
    EXPECT_EQ(cancelled.result, nullptr);
    EXPECT_TRUE(backend->paused);
    auto next = std::make_shared<FakeState>();
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(next)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.Finish(); controller.Join();
    EXPECT_EQ(controller.Snapshot().result->height, 500);
    EXPECT_EQ(controller.Snapshot().preview, nullptr);
}

TEST(ScrollCaptureControllerTest, FinishKeepsFirstFrameAndStopsBackend) {
    auto backend = std::make_shared<FakeState>();
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.Finish();
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Done));
    controller.Join();
    const auto snapshot = controller.Snapshot();
    ASSERT_NE(snapshot.result, nullptr);
    EXPECT_EQ(snapshot.result->height, 500);
    EXPECT_EQ(backend->stops, 1);
}

TEST(ScrollCaptureControllerTest, DefaultSessionUsesEnglishForFailureAndCompletion) {
    auto backend = std::make_shared<FakeState>();
    backend->stableImage = false;
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    EXPECT_EQ(controller.Snapshot().message, "Scroll inside the selection; click Done to finish.");
    controller.AddIntent(controller.Snapshot().epoch, 100);
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Failed));
    const auto failed = controller.Snapshot();
    EXPECT_NE(failed.message.find("The image is still changing"), std::string::npos);
    EXPECT_NE(failed.message.find("scroll to retry"), std::string::npos);
    EXPECT_EQ(failed.totalHeight, 500);
    controller.Finish();
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Done));
    controller.Join();
    EXPECT_EQ(controller.Snapshot().message, "Capture complete");
    ASSERT_NE(controller.Snapshot().result, nullptr);
    EXPECT_EQ(controller.Snapshot().result->height, 500);
    EXPECT_EQ(backend->stops, 1);
}

TEST(ScrollCaptureControllerTest, EnglishSessionDoesNotExposeChineseStitchDiagnostics) {
    auto backend = std::make_shared<FakeState>();
    backend->blankImage = true;
    ScrollCaptureController controller(Language::English);
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.AddIntent(controller.Snapshot().epoch, 100);
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Failed));
    const auto message = controller.Snapshot().message;
    EXPECT_NE(message.find("Not enough image detail"), std::string::npos);
    for (unsigned char ch : message) EXPECT_LT(ch, 128);
    controller.Finish();
    controller.Join();
}

TEST(ScrollCaptureControllerTest, UpwardSessionExtendsImageAboveInitialFrame) {
    auto backend = std::make_shared<FakeState>();
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.AddIntent(controller.Snapshot().epoch, -100);
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (controller.Snapshot().totalHeight <= 500 && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    ASSERT_GT(controller.Snapshot().totalHeight, 500);
    controller.Finish(); controller.Join();
    const auto snapshot = controller.Snapshot();
    ASSERT_EQ(snapshot.state, ScrollCaptureState::Done);
    ASSERT_NE(snapshot.result, nullptr);
    EXPECT_EQ(snapshot.result->height, 625);
    EXPECT_EQ(backend->injections, std::vector<int64_t>{-125});
}

TEST(ScrollCaptureControllerTest, WheelMagnitudeDoesNotChangeStepOrActualStitch) {
    // 极小输入和大幅输入都触发同一步长；靠近边界时按实际位移拼接。
    auto backend = std::make_shared<FakeState>();
    backend->y = 180;
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    int expectedHeight = 625;
    for (const int64_t delta : {-2, -10000}) {
        controller.AddIntent(controller.Snapshot().epoch, delta);
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (controller.Snapshot().totalHeight < expectedHeight &&
               std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        EXPECT_EQ(controller.Snapshot().totalHeight, expectedHeight);
        expectedHeight = 680;
    }
    controller.Finish(); controller.Join();
    EXPECT_EQ(backend->injections, (std::vector<int64_t>{-125, -125}));
    ASSERT_NE(controller.Snapshot().result, nullptr);
    scrolltest::FrameSpec spec;
    spec.viewportHeight = 680;
    EXPECT_EQ(controller.Snapshot().result->rgba,
              scrolltest::CutFrame(backend->page, 200, 2000, spec));
}

TEST(ScrollCaptureControllerTest, InputsDuringSuccessfulStepDoNotQueueMoreScrolling) {
    auto backend = std::make_shared<FakeState>();
    backend->blocked = true;
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    const auto epoch = controller.Snapshot().epoch;
    controller.AddIntent(epoch, 2);
    EXPECT_TRUE(WaitEntered(backend));
    for (int i = 0; i < 20; ++i) controller.AddIntent(epoch, 1000);
    Release(backend);
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (controller.Snapshot().totalHeight == 500 &&
           std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    controller.Finish(); controller.Join();
    EXPECT_EQ(backend->injections, (std::vector<int64_t>{125}));
    EXPECT_EQ(controller.Snapshot().totalHeight, 625);
}

// 每次反向输入只通知一次，后续轮询不应重复触发警告。
TEST(ReverseNoticeTest, ReverseNoticeIsForwardedOnlyOnce) {
    std::atomic<bool> reversed{true};
    EXPECT_EQ(ConsumeReverseNotice(reversed, 1), -1);   // 首次：转发反向 intent
    EXPECT_EQ(ConsumeReverseNotice(reversed, 1), 0);    // 之后：不再转发
    EXPECT_EQ(ConsumeReverseNotice(reversed, -1), 0);

    // 转发方向与首方向相反
    std::atomic<bool> reversedDown{true};
    EXPECT_EQ(ConsumeReverseNotice(reversedDown, -1), 1);

    // 未反转不转发；首方向未知（理论不可能）也不转发
    std::atomic<bool> untouched{false};
    EXPECT_EQ(ConsumeReverseNotice(untouched, 1), 0);
    std::atomic<bool> noDirection{true};
    EXPECT_EQ(ConsumeReverseNotice(noDirection, 0), 0);
}

TEST(ScrollCaptureControllerTest, OppositeIntentIsBlockedAndOriginalDirectionCanContinue) {
    for (const int direction : {-1, 1}) {
        auto backend = std::make_shared<FakeState>();
        backend->y = 600;
        ScrollCaptureController controller;
        ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
        ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
        const auto epoch = controller.Snapshot().epoch;
        controller.AddIntent(epoch, direction);
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (controller.Snapshot().totalHeight < 625 && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ASSERT_EQ(controller.Snapshot().totalHeight, 625);
        // 发布结果之后等待本步清理完成，再测试下一次独立输入。
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        controller.AddIntent(epoch, -direction);
        const auto warning = controller.Snapshot();
        EXPECT_EQ(warning.state, ScrollCaptureState::Capturing);
        EXPECT_TRUE(warning.reverseScrollBlocked);
        EXPECT_FALSE(controller.IsStopped());
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
        EXPECT_EQ(controller.Snapshot().totalHeight, 625);
        controller.AddIntent(epoch, direction);
        EXPECT_FALSE(controller.Snapshot().reverseScrollBlocked);
        until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (controller.Snapshot().totalHeight < 750 && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        controller.Finish(); controller.Join();
        ASSERT_NE(controller.Snapshot().result, nullptr);
        EXPECT_EQ(controller.Snapshot().result->height, 750);
        EXPECT_EQ(backend->injections, (std::vector<int64_t>{direction * 125, direction * 125}));
        scrolltest::FrameSpec spec;
        spec.contentY = direction > 0 ? 600 : 350;
        spec.viewportHeight = 750;
        EXPECT_EQ(controller.Snapshot().result->rgba,
                  scrolltest::CutFrame(backend->page, 200, 2000, spec));
    }
}

TEST(ScrollCaptureControllerTest, ReverseIntentDuringCaptureKeepsInFlightFrameAndWarning) {
    auto backend = std::make_shared<FakeState>(); backend->blocked = true;
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    const auto epoch = controller.Snapshot().epoch;
    controller.AddIntent(epoch, 100);
    EXPECT_TRUE(WaitEntered(backend));
    controller.AddIntent(epoch, -100);
    EXPECT_TRUE(controller.Snapshot().reverseScrollBlocked);
    Release(backend);
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (controller.Snapshot().totalHeight < 625 && std::chrono::steady_clock::now() < until)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_EQ(controller.Snapshot().state, ScrollCaptureState::Capturing);
    EXPECT_TRUE(controller.Snapshot().reverseScrollBlocked);
    controller.Finish(); controller.Join();
    ASSERT_NE(controller.Snapshot().result, nullptr);
    EXPECT_EQ(controller.Snapshot().result->height, 625);
    EXPECT_EQ(backend->injections, std::vector<int64_t>{125});
}

TEST(ScrollCaptureControllerTest, CorrectDirectionClearsWarningDuringInFlightCapture) {
    auto backend = std::make_shared<FakeState>(); backend->blocked = true;
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    const auto epoch = controller.Snapshot().epoch;
    controller.AddIntent(epoch, 100);
    EXPECT_TRUE(WaitEntered(backend));
    controller.AddIntent(epoch, -100);
    EXPECT_TRUE(controller.Snapshot().reverseScrollBlocked);
    controller.AddIntent(epoch, 100);
    EXPECT_FALSE(controller.Snapshot().reverseScrollBlocked);
    Release(backend);
    controller.Finish(); controller.Join();
    EXPECT_FALSE(controller.Snapshot().reverseScrollBlocked);
    EXPECT_EQ(backend->injections, std::vector<int64_t>{125});
}

TEST(ScrollCaptureControllerTest, ReverseImageMovementFailsWithoutStitching) {
    auto backend = std::make_shared<FakeState>(); backend->reverseMovement = true;
    ScrollCaptureController controller(Language::ChineseSimplified);
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.AddIntent(controller.Snapshot().epoch, 100);
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Failed));
    EXPECT_NE(controller.Snapshot().message.find("无法可靠拼接"), std::string::npos);
    controller.Finish(); controller.Join();
    ASSERT_NE(controller.Snapshot().result, nullptr);
    EXPECT_EQ(controller.Snapshot().result->height, 500);
    EXPECT_EQ(backend->injections, (std::vector<int64_t>{125, 0}));
}

TEST(ScrollCaptureControllerTest, CancelInvalidatesInFlightResultAndEpoch) {
    auto backend = std::make_shared<FakeState>(); backend->blocked = true;
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    auto oldEpoch = controller.Snapshot().epoch;
    controller.AddIntent(oldEpoch, 100);
    EXPECT_TRUE(WaitEntered(backend));
    controller.Cancel();
    Release(backend);
    controller.Join();
    EXPECT_EQ(controller.Snapshot().state, ScrollCaptureState::Cancelled);
    EXPECT_EQ(controller.Snapshot().result, nullptr);
    auto next = std::make_shared<FakeState>();
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(next)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.AddIntent(oldEpoch, 100); // 旧会话回调不能驱动新会话。
    controller.Finish(); controller.Join();
    EXPECT_TRUE(next->injections.empty());
}

TEST(ScrollCaptureControllerTest, FinishWaitsForOneFlightAndRejectsNewIntent) {
    auto backend = std::make_shared<FakeState>(); backend->blocked = true;
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    auto epoch = controller.Snapshot().epoch;
    controller.AddIntent(epoch, 100);
    EXPECT_TRUE(WaitEntered(backend));
    controller.Finish();
    controller.AddIntent(epoch, 500);
    Release(backend); controller.Join();
    auto snapshot = controller.Snapshot();
    ASSERT_EQ(snapshot.state, ScrollCaptureState::Done);
    ASSERT_NE(snapshot.result, nullptr);
    EXPECT_EQ(snapshot.result->height, 625);
    EXPECT_EQ(backend->injections.size(), 1u);
}

TEST(ScrollCaptureControllerTest, LateFinishDiscardsStepAfterDeadline) {
    auto backend = std::make_shared<FakeState>(); backend->blocked = true;
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.AddIntent(controller.Snapshot().epoch, 100);
    EXPECT_TRUE(WaitEntered(backend));
    controller.Finish();
    std::this_thread::sleep_for(std::chrono::milliseconds(520));
    Release(backend); controller.Join();
    ASSERT_NE(controller.Snapshot().result, nullptr);
    EXPECT_EQ(controller.Snapshot().result->height, 500);
}

TEST(ScrollCaptureControllerTest, TargetChangeFailsBeforeInjectionAndKeepsResult) {
    auto backend = std::make_shared<FakeState>();
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    { std::lock_guard lock(backend->mutex); backend->targetValid = false; }
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Failed));
    controller.AddIntent(controller.Snapshot().epoch, 100);
    controller.Finish(); controller.Join();
    ASSERT_NE(controller.Snapshot().result, nullptr);
    EXPECT_EQ(controller.Snapshot().result->height, 500);
    EXPECT_TRUE(backend->injections.empty());
}

TEST(ScrollCaptureControllerTest, CaptureFailureDoesNotDrivePendingSteps) {
    auto backend = std::make_shared<FakeState>(); backend->failCapture = true;
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.AddIntent(controller.Snapshot().epoch, 1000);
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Failed));
    controller.Finish(); controller.Join();
    EXPECT_EQ(backend->injections.size(), 1u);
    ASSERT_NE(controller.Snapshot().result, nullptr);
}

TEST(ScrollCaptureControllerTest, CaptureFailureDropsIntentsQueuedDuringStep) {
    auto backend = std::make_shared<FakeState>();
    backend->blocked = true;
    backend->failCapture = true;
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    const auto epoch = controller.Snapshot().epoch;
    controller.AddIntent(epoch, 100);
    ASSERT_TRUE(WaitEntered(backend));
    controller.AddIntent(epoch, 100);
    Release(backend);
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Failed));
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    controller.Finish(); controller.Join();
    EXPECT_EQ(backend->injections, std::vector<int64_t>{125});
}

TEST(ScrollCaptureControllerTest, BlankFramePausesAndPreservesOverlap) {
    // 持续空白最多推进到重叠预算边界，保留首帧后暂停。
    auto backend = std::make_shared<FakeState>(); backend->blankImage = true;
    ScrollCaptureController controller(Language::ChineseSimplified);
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.AddIntent(controller.Snapshot().epoch, 1000);
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Failed));
    EXPECT_NE(controller.Snapshot().message.find("原地重试"), std::string::npos);
    controller.Finish(); controller.Join();
    ASSERT_NE(controller.Snapshot().result, nullptr);
    EXPECT_EQ(controller.Snapshot().result->height, 500);
    // 重取一次画面，但不再次注入滚动，避免丢失重叠区域。
    EXPECT_EQ(backend->injections, (std::vector<int64_t>{125, 0, 175}));
}

TEST(ScrollCaptureControllerTest, TransientBlankAfterSuccessfulStepsRetriesWithoutScrolling) {
    // 图片加载期间短暂空白，必须在同一位置重取，保留与上一成功帧的重叠。
    auto backend = std::make_shared<FakeState>();
    backend->y = 600;
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    for (int step = 1; step <= 3; ++step) {
        if (step == 3) {
            std::lock_guard lock(backend->mutex);
            backend->blankImageCount = 1;
        }
        controller.AddIntent(controller.Snapshot().epoch, -100);
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (controller.Snapshot().totalHeight < 500 + step * 125 &&
               std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        EXPECT_EQ(controller.Snapshot().totalHeight, 500 + step * 125);
    }
    controller.Finish(); controller.Join();
    EXPECT_EQ(backend->injections, (std::vector<int64_t>{-125, -125, -125, 0}));
    const auto result = controller.Snapshot().result;
    ASSERT_NE(result, nullptr);
    scrolltest::FrameSpec spec;
    spec.contentY = 225;
    spec.viewportHeight = 875;
    EXPECT_EQ(result->rgba, scrolltest::CutFrame(backend->page, 200, 2000, spec));
}

TEST(ScrollCaptureControllerTest, MatchFailureWaitsForInputCleanupBeforePublishingRetryableState) {
    auto backend = std::make_shared<FakeState>();
    backend->badImageCount = FailedStepCaptureCount;
    auto gate = std::make_shared<StepCleanupGate>();
    auto cleanupEntered = gate->entered.get_future();
    ScrollCaptureController controller;
    // 断言提前退出也先解除后端阻塞，再等待控制线程析构，避免测试自身死锁。
    ScopeExit releaseCleanup([&] { gate->Release(); });
    ASSERT_TRUE(controller.Start(std::make_unique<CleanupGatedBackend>(backend, gate)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    const auto initialHeight = controller.Snapshot().totalHeight;

    controller.AddIntent(controller.Snapshot().epoch, 1);
    ASSERT_EQ(cleanupEntered.wait_for(ControllerWaitTimeout), std::future_status::ready);
    EXPECT_EQ(controller.Snapshot().state, ScrollCaptureState::Capturing);

    gate->Release();
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Failed));
    // Failed 一旦可见，重试就必须被接受，不能仍被步骤执行标志丢弃。
    controller.AddIntent(controller.Snapshot().epoch, 1);
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.Finish();
    controller.Join();
    ASSERT_NE(controller.Snapshot().result, nullptr);
    EXPECT_GT(controller.Snapshot().result->height, initialHeight);
    EXPECT_EQ(backend->injections.size(), size_t(FailedStepCaptureCount + 1));
}

TEST(ScrollCaptureControllerTest, CaptureFailureWaitsForInputCleanupBeforePublishingRetryableState) {
    constexpr size_t ExpectedCaptureCount = 2;
    auto backend = std::make_shared<FakeState>();
    backend->failCapture = true;
    auto gate = std::make_shared<StepCleanupGate>();
    auto cleanupEntered = gate->entered.get_future();
    ScrollCaptureController controller;
    ScopeExit releaseCleanup([&] { gate->Release(); });
    ASSERT_TRUE(controller.Start(std::make_unique<CleanupGatedBackend>(backend, gate)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    const auto initialHeight = controller.Snapshot().totalHeight;

    controller.AddIntent(controller.Snapshot().epoch, 1);
    ASSERT_EQ(cleanupEntered.wait_for(ControllerWaitTimeout), std::future_status::ready);
    EXPECT_EQ(controller.Snapshot().state, ScrollCaptureState::Capturing);
    {
        std::lock_guard lock(backend->mutex);
        backend->failCapture = false;
    }
    gate->Release();
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Failed));
    controller.AddIntent(controller.Snapshot().epoch, 1);
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.Finish();
    controller.Join();
    ASSERT_NE(controller.Snapshot().result, nullptr);
    EXPECT_EQ(controller.Snapshot().result->height, initialHeight);
    EXPECT_EQ(backend->injections.size(), ExpectedCaptureCount);
    EXPECT_EQ(backend->injections.back(), 0);
}

TEST(ScrollCaptureControllerTest, CompletedFrameWaitsForInputCleanupBeforePublishingNewHeight) {
    constexpr size_t ExpectedCaptureCount = 2;
    auto backend = std::make_shared<FakeState>();
    auto gate = std::make_shared<StepCleanupGate>();
    auto cleanupEntered = gate->entered.get_future();
    ScrollCaptureController controller;
    ScopeExit releaseCleanup([&] { gate->Release(); });
    ASSERT_TRUE(controller.Start(std::make_unique<CleanupGatedBackend>(backend, gate)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    const auto initialHeight = controller.Snapshot().totalHeight;

    controller.AddIntent(controller.Snapshot().epoch, 1);
    ASSERT_EQ(cleanupEntered.wait_for(ControllerWaitTimeout), std::future_status::ready);
    EXPECT_EQ(controller.Snapshot().totalHeight, initialHeight);
    gate->Release();
    const auto deadline = std::chrono::steady_clock::now() + ControllerWaitTimeout;
    while (controller.Snapshot().totalHeight == initialHeight &&
           std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(ControllerPollInterval);
    const auto completedHeight = controller.Snapshot().totalHeight;
    ASSERT_GT(completedHeight, initialHeight);
    {
        std::lock_guard lock(backend->mutex);
        backend->entered = false;
    }
    controller.AddIntent(controller.Snapshot().epoch, 1);
    ASSERT_TRUE(WaitEntered(backend));
    controller.Finish();
    controller.Join();
    ASSERT_NE(controller.Snapshot().result, nullptr);
    EXPECT_GT(controller.Snapshot().result->height, completedHeight);
    EXPECT_EQ(backend->injections.size(), ExpectedCaptureCount);
}

TEST(ScrollCaptureControllerTest, CancellationDuringInputCleanupCannotPublishLateFailure) {
    auto backend = std::make_shared<FakeState>();
    backend->badImageCount = FailedStepCaptureCount;
    auto gate = std::make_shared<StepCleanupGate>();
    auto cleanupEntered = gate->entered.get_future();
    ScrollCaptureController controller;
    ScopeExit releaseCleanup([&] { gate->Release(); });
    ASSERT_TRUE(controller.Start(std::make_unique<CleanupGatedBackend>(backend, gate)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));

    controller.AddIntent(controller.Snapshot().epoch, 1);
    ASSERT_EQ(cleanupEntered.wait_for(ControllerWaitTimeout), std::future_status::ready);
    controller.Cancel();
    const auto cancelledEpoch = controller.Snapshot().epoch;
    gate->Release();
    controller.Join();

    EXPECT_EQ(controller.Snapshot().state, ScrollCaptureState::Cancelled);
    EXPECT_EQ(controller.Snapshot().epoch, cancelledEpoch);
    EXPECT_EQ(controller.Snapshot().result, nullptr);
    EXPECT_EQ(backend->stops, 1);
}

TEST(ScrollCaptureControllerTest, FailedStepRecoversWhenUserScrollsAgain) {
    // 失败后滚轮只触发原地取帧，恢复成功后才允许下一次固定步长滚动。
    auto backend = std::make_shared<FakeState>(); backend->badImageCount = 2;
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.AddIntent(controller.Snapshot().epoch, 300);
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Failed));
    // 用户触发重试：应用保持当前位置，重新匹配最后一次滚动。
    controller.AddIntent(controller.Snapshot().epoch, 300);
    EXPECT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.Finish(); controller.Join();
    auto snapshot = controller.Snapshot();
    ASSERT_EQ(snapshot.state, ScrollCaptureState::Done);
    ASSERT_NE(snapshot.result, nullptr);
    EXPECT_EQ(snapshot.result->height, 625);
    EXPECT_EQ(backend->injections, (std::vector<int64_t>{125, 0, 0}));
}

TEST(ScrollCaptureControllerTest, PersistentFailureRestoresInjectionAfterInPlaceRetryFails) {
    // 原地重试步也失败后，用户的下一次滚动必须恢复注入：in-place 恢复若永不复位，
    // inject 被永久锁死为 0，用户滚动永远无法驱动页面（失败死锁）。
    auto backend = std::make_shared<FakeState>(); backend->badImageCount = 4;
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    // 第一步：注入 125，取帧+同步重试均坏 → Failed（injections {125, 0}）
    controller.AddIntent(controller.Snapshot().epoch, 300);
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Failed));
    // 第二步：原地重试（inject=0），取帧+同步重试均坏 → 仍 Failed。
    // 状态已是 Failed 无跳变，以 revision 递增等待本步完成（避免在 stepInFlight 中丢失下一个 intent）。
    const auto waitRevision = [&](uint64_t rev) {
        auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (std::chrono::steady_clock::now() < until) {
            if (controller.Snapshot().revision > rev) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return false;
    };
    const uint64_t rev = controller.Snapshot().revision;
    controller.AddIntent(controller.Snapshot().epoch, 300);
    ASSERT_TRUE(waitRevision(rev));
    // 第三步：用户滚动必须恢复注入 125（旧实现此处仍是 0），随后匹配成功回到 Capturing
    controller.AddIntent(controller.Snapshot().epoch, 300);
    EXPECT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.Finish(); controller.Join();
    const auto snapshot = controller.Snapshot();
    ASSERT_EQ(snapshot.state, ScrollCaptureState::Done);
    ASSERT_NE(snapshot.result, nullptr);
    EXPECT_EQ(snapshot.result->height, 750);
    std::lock_guard lock(backend->mutex);
    EXPECT_EQ(backend->injections, (std::vector<int64_t>{125, 0, 0, 0, 125}));
}

TEST(ScrollCaptureControllerTest, UnstableFramePausesAndPolledIntentRetriesInPlace) {
    // 模拟平台在 ValidateTarget 中转发 tap 输入，暂停后仍必须能收到重试。
    auto backend = std::make_shared<FakeState>();
    backend->stableImage = false;
    ScrollCaptureController controller(Language::ChineseSimplified);
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.AddIntent(controller.Snapshot().epoch, 2);
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Failed));
    EXPECT_EQ(controller.Snapshot().totalHeight, 500);
    EXPECT_NE(controller.Snapshot().message.find("尚未稳定"), std::string::npos);
    {
        std::lock_guard lock(backend->mutex);
        backend->stableImage = true;
        backend->polledIntent = 2;
    }
    EXPECT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.Finish(); controller.Join();
    EXPECT_EQ(controller.Snapshot().totalHeight, 625);
    EXPECT_EQ(backend->injections, (std::vector<int64_t>{125, 0, 0}));
}

TEST(ScrollCaptureControllerTest, MatchFailureRetriesWithoutInjectingAndPreservesFirstFrame) {
    auto backend = std::make_shared<FakeState>(); backend->badImage = true;
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    auto first = controller.Snapshot().viewport;
    controller.AddIntent(controller.Snapshot().epoch, 1000);
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Failed));
    controller.Finish(); controller.Join();
    ASSERT_EQ(backend->injections.size(), 2u);
    EXPECT_EQ(backend->injections[0], 125);
    EXPECT_EQ(backend->injections[1], 0);
    ASSERT_NE(controller.Snapshot().result, nullptr);
    EXPECT_EQ(controller.Snapshot().result->rgba, first->rgba);
}

TEST(ScrollCaptureControllerTest, FinishingFailedPreparationDoesNotHang) {
    // 准备阶段收到非法首帧时，保留明确失败状态供 UI 恢复普通截图。
    class InvalidBackend : public ScrollCaptureBackend {
    public:
        bool Start(ScrollImage& image, std::function<void(int64_t)>, std::string&) override {
            image = {}; return true;
        }
        bool CaptureStep(int64_t, ScrollImage&, bool&, std::string&) override { return false; }
        bool ValidateTarget(std::string&) override { return true; }
        void Stop() override {}
    };
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<InvalidBackend>()));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Failed));
    controller.Join();
    controller.Finish();
    EXPECT_EQ(controller.Snapshot().state, ScrollCaptureState::Failed);
    EXPECT_EQ(controller.Snapshot().viewport, nullptr);
}

TEST(ScrollTargetTest, MovementClosureOcclusionAndProcessChangeAreRejected) {
    ScrollTargetWindow original{42, 12, {0, 0, 800, 600}};
    GlobalRect selection{100, 100, 400, 400};
    ScrollTargetWindow locked;
    EXPECT_TRUE(ValidateScrollTarget({original}, locked, selection, 200, 200));
    auto moved = original; moved.bounds.x += 1;
    EXPECT_FALSE(ValidateScrollTarget({moved}, locked, selection, 200, 200));
    EXPECT_FALSE(ValidateScrollTarget({}, locked, selection, 200, 200));
    EXPECT_FALSE(ValidateScrollTarget({{50, 13, {450, 450, 100, 100}}, original}, locked, selection, 200, 200));
    EXPECT_TRUE(ValidateScrollTarget({{50, 13, {900, 900, 100, 100}}, original}, locked, selection, 200, 200));
    auto reused = original; reused.pid = 99;
    EXPECT_FALSE(ValidateScrollTarget({reused}, locked, selection, 200, 200));
}

TEST(ScrollPreviewTest, ThumbnailIsBoundedAndReadsBothEnds) {
    auto page = scrolltest::MakeTexturedLongImage(200, 5000, 7);
    LongImageStore store(200, 128);
    ASSERT_EQ(store.Initialize({page.data(), 200, 5000, 800}), AppendResult::Ok);
    auto preview = MakeScrollPreview(store, 180, 600, true);
    EXPECT_LE(preview.width, 180); EXPECT_LE(preview.height, 600);
    ASSERT_FALSE(preview.rgba.empty());
    EXPECT_EQ(preview.rgba.front(), page.front());
    EXPECT_LE(preview.rgba.size(), 180u * 600 * 4);
    EXPECT_GE(preview.width, 64);
    auto bottom = MakeScrollPreview(store);
    EXPECT_NE(preview.rgba, bottom.rgba);
}

TEST(LongImageOutputTest, OcrSlicesHaveOverlapAndUniqueOwnership) {
    auto slices = PlanOcrSlices(16384);
    ASSERT_GT(slices.size(), 1u);
    int previous = 0;
    for (const auto& slice : slices) {
        EXPECT_LE(slice.height, 4096);
        EXPECT_EQ(slice.acceptTop, previous);
        previous = slice.acceptBottom;
    }
    EXPECT_EQ(previous, 16384);
    EXPECT_EQ(slices.back().top + slices.back().height, 16384);
}
TEST(LongImageOutputTest, PinFitsVisibleScreenWithoutChangingAspect) {
    auto size = FitImageToScreen(800, 8192, 1400, 800);
    EXPECT_DOUBLE_EQ(size.height, 800);
    EXPECT_NEAR(size.width / size.height, 800.0 / 8192, 0.00001);
}

TEST(ScrollPointerGuardTest, DragLeavingSelectionStillSwallowsReleaseAndControlsRemainClickable) {
    GlobalRect selection{100, 100, 400, 400};
    GlobalRect controls{300, 450, 150, 40};
    unsigned buttons = 0;
    EXPECT_TRUE(SuppressScrollPointer(selection, controls, 200, 200, 0, ScrollPointerAction::Down, buttons));
    EXPECT_TRUE(SuppressScrollPointer(selection, controls, 800, 800, 0, ScrollPointerAction::Drag, buttons));
    EXPECT_TRUE(SuppressScrollPointer(selection, controls, 800, 800, 0, ScrollPointerAction::Up, buttons));
    EXPECT_EQ(buttons, 0u);
    EXPECT_FALSE(SuppressScrollPointer(selection, controls, 350, 470, 0, ScrollPointerAction::Down, buttons));
    EXPECT_FALSE(SuppressScrollPointer(selection, controls, 350, 470, 0, ScrollPointerAction::Up, buttons));
    EXPECT_FALSE(SuppressScrollPointer(selection, controls, 10, 10, 1, ScrollPointerAction::Down, buttons));
    EXPECT_TRUE(SuppressScrollPointer(selection, controls, 200, 200, 1, ScrollPointerAction::Down, buttons));
    EXPECT_TRUE(SuppressScrollPointer(selection, controls, 200, 200, 1, ScrollPointerAction::Up, buttons));
}

TEST(ScrollCaptureControllerTest, BlankRecoveryUsesCumulativeShiftAndPreservesPixels) {
    for (const int direction : {-1, 1}) {
        auto backend = std::make_shared<FakeState>();
        backend->y = 600;
        backend->blankImageCount = 2;
        ScrollCaptureController controller;
        ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
        ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
        controller.AddIntent(controller.Snapshot().epoch, direction);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        while (controller.Snapshot().totalHeight == 500 &&
               controller.Snapshot().state != ScrollCaptureState::Failed &&
               std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        EXPECT_EQ(controller.Snapshot().totalHeight, 800);
        controller.Finish(); controller.Join();
        const auto result = controller.Snapshot().result;
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(backend->injections, (std::vector<int64_t>{direction * 125, 0, direction * 175}));
        const int origin = direction > 0 ? 600 : 300;
        ASSERT_EQ(result->height, 800);
        EXPECT_EQ(std::memcmp(result->rgba.data(), backend->page.data() + origin * 200 * 4,
                              result->rgba.size()), 0);
    }
}

TEST(ScrollCaptureControllerTest, PersistentBlankStopsAfterTwoExtraAttempts) {
    auto backend = std::make_shared<FakeState>();
    backend->blankImage = true;
    ScrollCaptureController controller;
    StitcherConfig config;
    config.minOverlapRows = 40;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend), config));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.AddIntent(controller.Snapshot().epoch, 1);
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Failed));
    // 125 + 187 + 148 = 460，第三次推进被最小重叠预算截断。
    const auto revision = controller.Snapshot().revision;
    controller.AddIntent(controller.Snapshot().epoch, 1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (controller.Snapshot().revision == revision && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_GT(controller.Snapshot().revision, revision);
    controller.Finish(); controller.Join();
    EXPECT_EQ(backend->injections, (std::vector<int64_t>{125, 0, 187, 148, 0, 0}));
    ASSERT_NE(controller.Snapshot().result, nullptr);
    EXPECT_EQ(controller.Snapshot().result->height, 500);
}

TEST(ScrollCaptureControllerTest, FinishDuringBlankRecoveryKeepsAcceptedImage) {
    auto backend = std::make_shared<FakeState>();
    backend->blankImageCount = 2;
    backend->blockOnCapture = 3;
    ScrollCaptureController controller;
    ASSERT_TRUE(controller.Start(std::make_unique<FakeBackend>(backend)));
    ASSERT_TRUE(WaitState(controller, ScrollCaptureState::Capturing));
    controller.AddIntent(controller.Snapshot().epoch, 1);
    {
        std::unique_lock lock(backend->mutex);
        EXPECT_TRUE(backend->cv.wait_for(lock, std::chrono::seconds(3), [&] {
            return backend->injections.size() >= 3;
        }));
    }
    controller.Finish();
    Release(backend);
    controller.Join();
    const auto snapshot = controller.Snapshot();
    ASSERT_EQ(snapshot.state, ScrollCaptureState::Done);
    ASSERT_NE(snapshot.result, nullptr);
    EXPECT_EQ(snapshot.result->height, 500);
    EXPECT_EQ(backend->injections, (std::vector<int64_t>{125, 0, 175}));
}
