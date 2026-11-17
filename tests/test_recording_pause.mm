#include "app/Application.h"
#include "platform/ApplicationPlatform.h"
#include "utils/ImGuiUtils.h"
#include "video_decode_mac.h"
#include "image_decode_mac.h"
#include <gtest/gtest.h>
#include <fstream>
#include <imgui_internal.h>
#include <iostream>

using namespace shotlet;
using namespace shotlet::TestUtils;
namespace {
constexpr double SegmentSeconds = 0.5, PausedSeconds = 0.7, DisplaySettleSeconds = 0.1;
constexpr double ExpectedActiveSeconds = SegmentSeconds * 3, DurationToleranceSeconds = 0.5;
constexpr int WidthLogical = 320, HeightLogical = 240, PixelChannels = 4;
constexpr int ColorThreshold = 150, ColorDifference = 80;

void VerifyNoPausedColor(const std::array<unsigned char, 3>& color) {
    // 暂停期间的绿色不能进入成片；红色和蓝色分别证明暂停前后都有有效画面。
    EXPECT_FALSE(color[1] > ColorThreshold && color[1] > color[0] + ColorDifference && color[1] > color[2] + ColorDifference);
}

void VerifyPauseCapture(RecordingFormat format, bool audio) {
    if (!CGPreflightScreenCaptureAccess()) GTEST_SKIP() << "需要屏幕录制权限";
    if (audio && [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] != AVAuthorizationStatusAuthorized)
        GTEST_SKIP() << "需要麦克风权限";
    ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO));
    const auto display = CGMainDisplayID();
    const auto bounds = CGDisplayBounds(display);
    RecordingOptions options;
    ASSERT_TRUE(PrepareRecordingOptions(display, {bounds.origin.x + 100, bounds.origin.y + 100, WidthLogical, HeightLogical}, 1, options));
    options.format = format; options.systemAudio = options.microphone = audio; options.showCursor = false;
    auto* panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(options.region.x,
        CGYToAppKitBottomY(options.region.y, options.region.height, bounds.size.height), WidthLogical, HeightLogical)
        styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    panel.releasedWhenClosed = NO; panel.hidesOnDeactivate = NO; panel.level = NSFloatingWindowLevel;
    panel.backgroundColor = NSColor.redColor; [panel orderFrontRegardless];
    const ScopeExit close([&] { [panel close]; });
    const auto directory = std::filesystem::temp_directory_path() / ("shotlet-pause-" + std::string(NSUUID.UUID.UUIDString.UTF8String));
    std::filesystem::create_directory(directory);
    const ScopeExit remove([&] { std::error_code ignored; std::filesystem::remove_all(directory, ignored); });
    const auto path = directory / (std::string("录制暂停") + RecordingExtension(format));
    RecordingSession session;
    std::string error;
    auto start = std::async(std::launch::async, [&] { return session.Start(options, path, error); });
    ASSERT_TRUE(Await(start)) << error;
    const auto pause = [&](bool paused) {
        auto task = std::async(std::launch::async, [&] { return session.SetPaused(paused, error); });
        return Await(task);
    };
    PumpFor(SegmentSeconds);
    for (auto* color : {NSColor.blueColor, NSColor.redColor}) {
        ASSERT_TRUE(pause(true)) << error;
        EXPECT_TRUE(session.IsPaused()); EXPECT_FALSE(session.HasStopped());
        panel.backgroundColor = NSColor.greenColor; PumpFor(PausedSeconds);
        panel.backgroundColor = color; PumpFor(DisplaySettleSeconds);
        ASSERT_TRUE(pause(false)) << error;
        EXPECT_FALSE(session.IsPaused()); PumpFor(SegmentSeconds);
    }
    ASSERT_TRUE(pause(true)) << error;
    panel.backgroundColor = NSColor.greenColor; PumpFor(PausedSeconds);
    auto stop = std::async(std::launch::async, [&] { return session.Stop(error); });
    ASSERT_TRUE(Await(stop)) << error;
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
    bool hasRed = false, hasBlue = false;
    double durationSeconds = 0;
    if (format == RecordingFormat::Mp4) {
        const auto frames = DecodeVideoFrames(path, error);
        ASSERT_FALSE(frames.empty()) << error;
        for (const auto& frame : frames) {
            VerifyNoPausedColor(frame.centerRgb);
            hasRed |= frame.centerRgb[0] > ColorThreshold; hasBlue |= frame.centerRgb[2] > ColorThreshold;
        }
        auto* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]] options:nil];
        durationSeconds = CMTimeGetSeconds(asset.duration);
        const auto videoTracks = LoadAssetTracks(asset, AVMediaTypeVideo, error);
        const auto audioTracks = LoadAssetTracks(asset, AVMediaTypeAudio, error);
        EXPECT_EQ(videoTracks.count, 1);
        EXPECT_EQ(audioTracks.count, audio ? 1 : 0);
        if (audio) {
            auto* track = audioTracks.firstObject;
            EXPECT_NEAR(CMTimeGetSeconds(track.timeRange.duration), durationSeconds, DurationToleranceSeconds);
        }
    } else {
        auto* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
        auto source = CGImageSourceCreateWithURL((__bridge CFURLRef)url, nullptr);
        ASSERT_NE(source, nullptr);
        const ScopeExit release([&] { CFRelease(source); });
        for (size_t index = 0; index < CGImageSourceGetCount(source); ++index) {
            int width = 0, height = 0;
            const auto pixels = DecodeImageFrame(path, index, width, height, error);
            ASSERT_FALSE(pixels.empty()) << error;
            const auto center = (size_t(height / 2) * width + width / 2) * PixelChannels;
            const std::array<unsigned char, 3> color{pixels[center], pixels[center + 1], pixels[center + 2]};
            VerifyNoPausedColor(color); hasRed |= color[0] > ColorThreshold; hasBlue |= color[2] > ColorThreshold;
            NSDictionary* properties = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, index, nullptr));
            durationSeconds += [properties[(id)kCGImagePropertyGIFDictionary][(id)kCGImagePropertyGIFUnclampedDelayTime] doubleValue];
        }
    }
    EXPECT_TRUE(hasRed); EXPECT_TRUE(hasBlue);
    EXPECT_NEAR(durationSeconds, ExpectedActiveSeconds, DurationToleranceSeconds);
}
}

TEST(RecordingPauseNativeTest, SilentMp4ExcludesPausedFramesAndSavesWhilePaused) { @autoreleasepool { VerifyPauseCapture(RecordingFormat::Mp4, false); } }
TEST(RecordingPauseNativeTest, AudioMp4KeepsAudioAndVideoTimelinesTogetherAcrossPauses) { @autoreleasepool { VerifyPauseCapture(RecordingFormat::Mp4, true); } }
TEST(RecordingPauseNativeTest, GifExcludesPausedFramesAndDelayAndSavesWhilePaused) { @autoreleasepool { VerifyPauseCapture(RecordingFormat::Gif, false); } }

namespace shotlet {
namespace {
class PauseFlowRecorder final : public IScreenRecorder {
public:
    bool failPause = false;
    std::shared_future<void> pauseGate;
    bool Start(const RecordingOptions&, const std::filesystem::path& path, std::string&) override {
        std::ofstream(path) << "completed-video"; return true;
    }
    bool Stop(std::string&) override { return true; }
    bool SetPaused(bool, std::string& error) override {
        if (pauseGate.valid()) pauseGate.wait();
        if (failPause) error = "模拟暂停失败";
        return !failPause;
    }
    std::string GetError() const override { return {}; }
};
}

class RecordingPauseFlowTest : public ::testing::Test {
protected:
    using State = RecordingWindow::State;
    void SetUp() override {
        ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO));
        SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
        m_app = std::make_unique<Application>();
        m_app->m_gpuDevice = CreateApplicationGpuDevice(false);
        ASSERT_NE(m_app->m_gpuDevice, nullptr);
        // 关闭自动预览，使用假编码器只验证应用层异步状态和自动保存。
        m_app->m_running = false; m_app->m_daemonMode = true;
        ASSERT_TRUE(PrepareRecordingOptions(CGMainDisplayID(), {100, 100, 640, 480}, 1, m_app->m_recordingOptions));
        m_app->m_recordingWindow = std::make_unique<RecordingWindow>(m_app->m_gpuDevice, Language::ChineseSimplified);
        ASSERT_TRUE(m_app->m_recordingWindow->Show(m_app->m_recordingOptions));
        m_app->m_recordingWindow->SetState(State::Recording);
        m_directory = std::filesystem::temp_directory_path() / NSUUID.UUID.UUIDString.UTF8String;
        std::filesystem::create_directory(m_directory);
        m_app->m_recordingPath = m_directory / "limit.mp4";
        auto recorder = std::make_unique<PauseFlowRecorder>();
        m_recorder = recorder.get();
        m_app->m_recordingSession = std::make_shared<RecordingSession>(std::move(recorder));
        std::string error;
        ASSERT_TRUE(m_app->m_recordingSession->Start(m_app->m_recordingOptions, m_app->m_recordingPath, error)) << error;
    }
    void TearDown() override {
        m_app.reset();
        std::error_code ignored; std::filesystem::remove_all(m_directory, ignored);
    }
    State CurrentState() const { return m_app->m_recordingWindow->GetState(); }
    void TogglePause() { m_app->ToggleRecordingPause(); }
    void Stop() { m_app->StopRecording(); }
    bool WaitForState(State state) {
        return PumpUntil([&] { m_app->ProcessRecording(); return CurrentState() == state; });
    }
    void SetElapsed(PausableClock::Clock::duration elapsed, bool paused) {
        auto& clock = m_app->m_recordingWindow->m_clock;
        const auto now = PausableClock::Clock::now();
        clock.Reset(now - elapsed); clock.SetPaused(paused, now);
    }
    bool LimitReached() const { return m_app->m_recordingWindow->HasReachedDurationLimit(); }
    bool Saved() const { return std::filesystem::exists(m_app->m_recordingPath); }
    void Frame() {
        const bool running = m_app->m_running;
        m_app->m_running = true;
        const ScopeExit restore([&] { m_app->m_running = running; });
        @autoreleasepool { m_app->ProcessFrame(); }
    }
    RecordingWindow& Controls() { return *m_app->m_recordingWindow; }
    std::unique_ptr<Application> m_app;
    PauseFlowRecorder* m_recorder = nullptr;
    std::filesystem::path m_directory;
};

TEST_F(RecordingPauseFlowTest, PausedTimeDoesNotTriggerLimitAndResumeAutomaticallySavesAtOneHour) {
    TogglePause(); EXPECT_EQ(CurrentState(), State::Pausing);
    ASSERT_TRUE(WaitForState(State::Paused));
    SetElapsed(MaximumRecordingDuration - std::chrono::milliseconds(1), true);
    PumpFor(DisplaySettleSeconds);
    EXPECT_FALSE(LimitReached()); EXPECT_FALSE(Saved());
    TogglePause();
    ASSERT_TRUE(WaitForState(State::Saved));
    EXPECT_TRUE(Saved());
}

TEST_F(RecordingPauseFlowTest, StopDuringPauseWaitsForNativeOperationAndSaves) {
    std::promise<void> gate;
    m_recorder->pauseGate = gate.get_future().share();
    TogglePause(); Stop();
    EXPECT_EQ(CurrentState(), State::Pausing);
    EXPECT_FALSE(Saved());
    gate.set_value();
    ASSERT_TRUE(WaitForState(State::Saved)); EXPECT_TRUE(Saved());
}

TEST_F(RecordingPauseFlowTest, PauseFailureReturnsToRecordingAndCanRetry) {
    m_recorder->failPause = true;
    TogglePause(); ASSERT_TRUE(WaitForState(State::Recording));
    EXPECT_FALSE(Saved());
    m_recorder->failPause = false;
    TogglePause(); ASSERT_TRUE(WaitForState(State::Paused));
    Stop(); ASSERT_TRUE(WaitForState(State::Saved)); EXPECT_TRUE(Saved());
}

TEST_F(RecordingPauseFlowTest, QueuedMouseMotionUpdatesToolbarHoverInTheSameFrame) {
    constexpr int SampleFrames = 60, WarmupFrames = 5;
    constexpr float PauseCenterX = 62, StopCenterX = 355, ControlCenterY = 26;
    auto& controls = Controls();
    for (int frame = 0; frame < WarmupFrames; ++frame) controls.Render(Theme::Dark);
    const ImGuiUtils::ContextScope context(controls.Context());
    auto& io = ImGui::GetIO(); io.ConfigInputTrickleEventQueue = false;
    auto* window = ImGui::FindWindowByName("##popup-surface"); ASSERT_NE(window, nullptr);
    const auto pauseId = window->GetID("record-pause");
    const auto stopId = window->GetID(Tr(Language::ChineseSimplified, TextId::RecordingEnd));
    SDL_Event button{}; button.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    button.button.windowID = SDL_GetWindowID(controls.Window()); button.button.button = SDL_BUTTON_RIGHT;
    // 保持按键事件序列，避免平台后端的全局鼠标轮询覆盖测试队列中的坐标。
    ASSERT_TRUE(SDL_PushEvent(&button));
    int delayedHoverFrames = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int frame = 0; frame < SampleFrames; ++frame) {
        const bool pause = frame % 2 == 0;
        SDL_Event motion{}; motion.type = SDL_EVENT_MOUSE_MOTION;
        motion.motion.windowID = SDL_GetWindowID(controls.Window());
        motion.motion.x = pause ? PauseCenterX : StopCenterX; motion.motion.y = ControlCenterY;
        ASSERT_TRUE(SDL_PushEvent(&motion));
        Frame();
        if (ImGui::GetCurrentContext()->HoveredId != (pause ? pauseId : stopId)) ++delayedHoverFrames;
    }
    const auto elapsedMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
    std::cout << "toolbar_hover average_frame_ms=" << elapsedMs / SampleFrames << " delayed_frames=" << delayedHoverFrames << '\n';
    EXPECT_EQ(delayedHoverFrames, 0);
    button.type = SDL_EVENT_MOUSE_BUTTON_UP; ASSERT_TRUE(SDL_PushEvent(&button)); Frame();
}
}
