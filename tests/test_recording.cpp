#include "recording/ScreenRecorder.h"
#include "utils/PcmMixBuffer.h"
#include "utils/FilePathUtils.h"
#include "recording/RecordingVisuals.h"
#include "platform/FileService.h"
#include "utils/WindowUtils.h"
#include "utils/PausableClock.h"
#include "utils/FormatUtils.h"
#include <gtest/gtest.h>
#include <array>
#include <chrono>
#include <fstream>
#include <limits>

using namespace shotlet;
namespace {
class RecordingTest : public testing::Test {
protected:
    void SetUp() override {
        directory = std::filesystem::temp_directory_path() /
            ("shotlet-recording-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(directory);
    }
    void TearDown() override { std::error_code ignored; std::filesystem::remove_all(directory, ignored); }
    std::string Read(const std::filesystem::path& path) {
        std::ifstream input(path, std::ios::binary);
        return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    }
    std::filesystem::path directory;
};
class FakeRecorder final : public IScreenRecorder {
public:
    bool failStart = false, failStop = false, failPause = false, paused = false;
    int pauseCalls = 0;
    bool Start(const RecordingOptions&, const std::filesystem::path& path, std::string& error) override {
        std::ofstream(path) << "new-video";
        if (failStart) error = "start failed";
        return !failStart;
    }
    bool Stop(std::string& error) override {
        if (failStop) error = "finish failed";
        return !failStop;
    }
    std::string GetError() const override { return {}; }
    bool SetPaused(bool value, std::string& error) override {
        ++pauseCalls;
        if (failPause) { error = "pause failed"; return false; }
        paused = value;
        return true;
    }
};
}

TEST(PausableClockTest, RepeatedPauseResumeExcludesIdleTimeAndKeepsOneHourBoundary) {
    using namespace std::chrono;
    const PausableClock::TimePoint epoch{};
    PausableClock clock;
    clock.Reset(epoch);
    clock.SetPaused(true, epoch + minutes(10));
    EXPECT_EQ(clock.Elapsed(epoch + hours(2)), minutes(10));
    clock.SetPaused(true, epoch + hours(2));
    clock.SetPaused(false, epoch + hours(3));
    EXPECT_EQ(clock.Elapsed(epoch + hours(3) + minutes(49)), minutes(59));
    clock.SetPaused(false, epoch + hours(3) + minutes(49));
    EXPECT_EQ(clock.Elapsed(epoch + hours(3) + minutes(50)), MaximumRecordingDuration);
    clock.SetPaused(true, epoch + hours(3) + minutes(50));
    EXPECT_EQ(clock.Elapsed(epoch + hours(10)), MaximumRecordingDuration);
    clock.Reset(epoch + hours(10));
    EXPECT_EQ(clock.Elapsed(epoch + hours(10)), seconds::zero());
    EXPECT_FALSE(clock.IsPaused());
    EXPECT_EQ(FormatUtils::HoursMinutesSeconds(seconds(9)), "00:00:09");
    EXPECT_EQ(FormatUtils::HoursMinutesSeconds(MaximumRecordingDuration), "01:00:00");
}

TEST_F(RecordingTest, PauseFailurePreservesStateAndPausedStopCommitsWithoutResuming) {
    auto recorder = std::make_unique<FakeRecorder>();
    auto* fake = recorder.get();
    RecordingSession session(std::move(recorder));
    std::string error;
    EXPECT_FALSE(session.SetPaused(true, error));
    const auto target = directory / "paused.mp4";
    ASSERT_TRUE(session.Start({}, target, error));
    fake->failPause = true;
    EXPECT_FALSE(session.SetPaused(true, error)); EXPECT_FALSE(session.IsPaused());
    fake->failPause = false;
    ASSERT_TRUE(session.SetPaused(true, error)); EXPECT_TRUE(session.IsPaused());
    const int calls = fake->pauseCalls;
    ASSERT_TRUE(session.SetPaused(true, error)); EXPECT_EQ(fake->pauseCalls, calls);
    fake->failPause = true;
    EXPECT_FALSE(session.SetPaused(false, error)); EXPECT_TRUE(session.IsPaused());
    fake->failPause = false;
    ASSERT_TRUE(session.SetPaused(false, error)); EXPECT_FALSE(session.IsPaused());
    ASSERT_TRUE(session.SetPaused(true, error));
    ASSERT_TRUE(session.Stop(error)) << error;
    EXPECT_TRUE(fake->paused); EXPECT_FALSE(session.IsPaused()); EXPECT_FALSE(session.IsActive());
    EXPECT_EQ(Read(target), "new-video");
    EXPECT_FALSE(session.SetPaused(false, error));
}

TEST(RecordingGeometryTest, ToolbarStaysOutsideLowerRightAndFitsNearScreenEdges) {
    constexpr int Gap = 12;
    const SDL_Rect work{-1920, 0, 1920, 1080};
    const SDL_Point toolbarSize{416, 52};
    const SDL_Rect region{-1800, 100, 800, 600};
    const auto normal = WindowUtils::PlaceAnchoredPopup(region, toolbarSize, work, Gap);
    EXPECT_EQ(normal.x + normal.w, region.x + region.w); EXPECT_EQ(normal.y, region.y + region.h + Gap);
    const SDL_Rect bottomRegion{-1800, 860, 800, 200};
    const auto above = WindowUtils::PlaceAnchoredPopup(bottomRegion, toolbarSize, work, Gap);
    EXPECT_EQ(above.x + above.w, bottomRegion.x + bottomRegion.w); EXPECT_EQ(above.y + above.h, bottomRegion.y - Gap);
    for (const auto anchor : {work, SDL_Rect{-1920, 0, 2, 2}}) {
        const auto popup = WindowUtils::PlaceAnchoredPopup(anchor, toolbarSize, work, Gap);
        EXPECT_GE(popup.x, work.x); EXPECT_GE(popup.y, work.y);
        EXPECT_LE(popup.x + popup.w, work.x + work.w); EXPECT_LE(popup.y + popup.h, work.y + work.h);
    }
}

TEST(RecordingOptionsTest, RetinaPixelsAreAlignedWithoutStretchingLogicalRegion) {
    RecordingOptions options;
    ASSERT_TRUE(PrepareRecordingOptions(7, {-800, 100, 201.5, 103.5}, 2, options));
    EXPECT_EQ(options.widthPixels, 402); EXPECT_EQ(options.heightPixels, 206);
    EXPECT_DOUBLE_EQ(options.region.x, -800); EXPECT_DOUBLE_EQ(options.region.y, 100);
    EXPECT_DOUBLE_EQ(options.region.width * 2, options.widthPixels);
    EXPECT_DOUBLE_EQ(options.region.height * 2, options.heightPixels);
}

TEST(RecordingOptionsTest, WindowsCoordinatesRemainActualPixelsAtHighDpi) {
    RecordingOptions options;
    ASSERT_TRUE(PrepareRecordingOptions(0, {-1920, -100, 801, 601}, 1, options));
    EXPECT_EQ(options.widthPixels, 800); EXPECT_EQ(options.heightPixels, 600);
    EXPECT_DOUBLE_EQ(options.region.width, 800);
}

TEST(RecordingOptionsTest, RejectsTinyNonfiniteAndOverflowingRegions) {
    RecordingOptions options;
    for (double density : {0.0, -1.0, std::numeric_limits<double>::infinity()})
        EXPECT_FALSE(PrepareRecordingOptions(0, {0, 0, 100, 100}, density, options));
    EXPECT_FALSE(PrepareRecordingOptions(0, {0, 0, 1, 100}, 1, options));
    EXPECT_FALSE(PrepareRecordingOptions(0, {0, 0, 1e100, 100}, 1, options));
    EXPECT_FALSE(PrepareRecordingOptions(0, {std::numeric_limits<double>::quiet_NaN(), 0, 100, 100}, 1, options));
    EXPECT_FALSE(PrepareRecordingOptions(-1, {0, 0, 100, 100}, 1, options));
}

TEST(PcmMixBufferTest, MixesTwoSourcesByTimestampAndFillsMissingAudioWithSilence) {
    PcmMixBuffer mix(2, 16);
    const std::array<float, 4> system{0.8f, -0.4f, 0.6f, -0.2f};
    const std::array<float, 4> microphone{0.2f, 0.2f, 0.4f, 0.4f};
    ASSERT_TRUE(mix.Add(system, 1, 0.5f));
    ASSERT_TRUE(mix.Add(microphone, 2, 0.5f));
    std::array<float, 10> result{}; mix.Read(result);
    const std::array<float, 10> expected{0, 0, 0.4f, -0.2f, 0.4f, 0, 0.2f, 0.2f, 0, 0};
    for (size_t i = 0; i < result.size(); ++i) EXPECT_NEAR(result[i], expected[i], 0.00001f);
}

TEST(PcmMixBufferTest, LateAndOutOfRangeSamplesDoNotContaminateRingAfterWrap) {
    PcmMixBuffer mix(2, 4);
    std::array<float, 8> discarded{}; mix.Read(discarded);
    const std::array<float, 4> samples{0.5f, 0.5f, 0.7f, 0.7f};
    ASSERT_TRUE(mix.Add(samples, 3, 1));
    ASSERT_FALSE(mix.Add(samples, 7, 1));
    std::array<float, 8> result{}; mix.Read(result);
    EXPECT_FLOAT_EQ(result[0], 0.7f); EXPECT_FLOAT_EQ(result[1], 0.7f);
    for (size_t i = 2; i < result.size(); ++i) EXPECT_FLOAT_EQ(result[i], 0);
}

TEST(PcmMixBufferTest, AudioCapturedBeforeVideoEpochIsDiscardedWithoutShiftingCurrentSamples) {
    constexpr int Channels = 2, CapacityFrames = 4;
    PcmMixBuffer mix(Channels, CapacityFrames);
    const std::array<float, 4> oldAudio{0.9f, 0.9f, 0.8f, 0.8f};
    ASSERT_TRUE(mix.Add(oldAudio, -CapacityFrames, 1));
    // 同一设备包跨过视频起点时，仅保留时间零之后的音频，不能把旧样本挤到视频开头。
    const std::array<float, 6> boundaryAudio{0.7f, 0.7f, 0.2f, -0.2f, 0.4f, -0.4f};
    ASSERT_TRUE(mix.Add(boundaryAudio, -1, 1));
    std::array<float, 6> output{}; mix.Read(output);
    const std::array<float, 6> expected{0.2f, -0.2f, 0.4f, -0.4f, 0, 0};
    EXPECT_EQ(output, expected);
}

TEST(PcmMixBufferTest, RejectsMalformedFramesAndSanitizesNonfiniteDeviceSamples) {
    EXPECT_THROW(PcmMixBuffer(0, 4), std::invalid_argument);
    EXPECT_THROW(PcmMixBuffer(2, 0), std::invalid_argument);
    PcmMixBuffer mix(2, 4);
    const std::array<float, 1> incomplete{1};
    EXPECT_FALSE(mix.Add(incomplete, 0, 1));
    const std::array<float, 2> invalid{std::numeric_limits<float>::quiet_NaN(), 0.5f};
    ASSERT_TRUE(mix.Add(invalid, 0, 1));
    std::array<float, 2> output{}; mix.Read(output);
    EXPECT_FLOAT_EQ(output[0], 0); EXPECT_FLOAT_EQ(output[1], 0.5f);
    EXPECT_TRUE(mix.Add(invalid, std::numeric_limits<int64_t>::min(), 1));
}

TEST(RecordingPathTest, UnicodeDisplayNameUsesUtf8OnEveryPlatform) {
    const auto path = std::filesystem::path(u8"录制 文件.mp4");
    EXPECT_EQ(FilePathUtils::ToUtf8(path), "录制 文件.mp4");
}

TEST_F(RecordingTest, DefaultOutputNamesAreUniqueAndNeverOverwriteExistingRecordings) {
    const auto folder = directory / std::filesystem::path(u8"默认录屏目录");
    std::string error;
    const auto first = CreateRecordingSavePath(folder, ".mp4", error);
    ASSERT_FALSE(first.empty()) << error;
    std::ofstream(first) << "existing";
    const auto second = CreateRecordingSavePath(folder, ".mp4", error);
    const auto gif = CreateRecordingSavePath(folder, ".gif", error);
    EXPECT_EQ(first.parent_path(), folder); EXPECT_EQ(second.parent_path(), folder);
    EXPECT_NE(first, second); EXPECT_EQ(gif.extension(), ".gif"); EXPECT_EQ(Read(first), "existing");
}

TEST(RecordingGeometryTest, OutlineStaysOutsideContentAndCameraFitsLowerLeftAtEveryScale) {
    for (double scale : {1.0, 1.5, 2.0}) {
        RecordingOptions options; options.region = {-800, 100, 700, 500};
        const auto camera = RecordingCameraRect(options, scale);
        EXPECT_DOUBLE_EQ(camera.width, camera.height);
        EXPECT_GE(camera.x, options.region.x); EXPECT_GE(camera.y, options.region.y);
        EXPECT_LE(camera.x + camera.width, options.region.x + options.region.width);
        EXPECT_LE(camera.y + camera.height, options.region.y + options.region.height);
        for (const auto border : OuterBorderRects(options.region, 2 * scale)) {
            const bool outside = border.x + border.width <= options.region.x || border.x >= options.region.x + options.region.width ||
                border.y + border.height <= options.region.y || border.y >= options.region.y + options.region.height;
            EXPECT_TRUE(outside);
        }
    }
    const auto popup = WindowUtils::PlaceCenteredPopup({-1800, 200, 800, 600}, {360, 192}, {-1920, 0, 1920, 1080});
    EXPECT_EQ(popup.x + popup.w / 2, -1400); EXPECT_EQ(popup.y + popup.h / 2, 500);
    const auto tiny = WindowUtils::PlaceCenteredPopup({-1920, 0, 2, 2}, {360, 192}, {-1920, 0, 1920, 1080});
    EXPECT_GE(tiny.x, -1920); EXPECT_GE(tiny.y, 0);
    const RectD display{-1920, 0, 1920, 1080};
    for (const auto strip : VisibleBorderRects(display, display, 2)) {
        EXPECT_GE(strip.x, display.x); EXPECT_GE(strip.y, display.y);
        EXPECT_LE(strip.x + strip.width, display.x + display.width);
        EXPECT_LE(strip.y + strip.height, display.y + display.height);
        EXPECT_GT(strip.width, 0); EXPECT_GT(strip.height, 0);
    }
}

TEST(RecordingOptionsTest, GifNeverRequestsAudioEvenWhenMp4AudioPreferencesAreEnabled) {
    RecordingOptions options;
    EXPECT_TRUE(options.HasAudio()); options.format = RecordingFormat::Gif; EXPECT_FALSE(options.HasAudio());
    options.format = RecordingFormat::Mp4; options.systemAudio = options.microphone = false; EXPECT_FALSE(options.HasAudio());
}

TEST(RecordingOptionsTest, PlaybackAspectFitPreservesWideAndTallFramesAtAnyScale) {
    const auto wide = AspectFitRect({100, 50, 800, 600}, 1920, 1080);
    EXPECT_DOUBLE_EQ(wide.x, 100); EXPECT_DOUBLE_EQ(wide.y, 125);
    EXPECT_DOUBLE_EQ(wide.width, 800); EXPECT_DOUBLE_EQ(wide.height, 450);
    const auto tall = AspectFitRect({0, 0, 800, 600}, 300, 600);
    EXPECT_DOUBLE_EQ(tall.x, 250); EXPECT_DOUBLE_EQ(tall.width, 300); EXPECT_DOUBLE_EQ(tall.height, 600);
    EXPECT_DOUBLE_EQ(AspectFitRect({0, 0, 800, 600}, 0, 600).width, 0);
}

TEST_F(RecordingTest, SuccessfulFinishReplacesExistingUnicodeFileOnlyAfterStop) {
    const auto target = directory / std::filesystem::path(u8"录屏 测试.mp4");
    std::ofstream(target) << "previous-video";
    RecordingSession session(std::make_unique<FakeRecorder>());
    std::string error;
    ASSERT_TRUE(session.Start({}, target, error));
    EXPECT_EQ(Read(target), "previous-video");
    ASSERT_TRUE(session.Stop(error)) << error;
    EXPECT_EQ(Read(target), "new-video");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
}

TEST_F(RecordingTest, FailedStartAndFailedFinalizationPreserveOriginalAndRemoveTemporaryFiles) {
    const auto target = directory / "recording.mp4";
    std::ofstream(target) << "previous-video";
    for (bool startFails : {true, false}) {
        auto recorder = std::make_unique<FakeRecorder>();
        recorder->failStart = startFails; recorder->failStop = !startFails;
        RecordingSession session(std::move(recorder));
        std::string error;
        const bool started = session.Start({}, target, error);
        EXPECT_EQ(started, !startFails);
        if (started) EXPECT_FALSE(session.Stop(error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(Read(target), "previous-video");
    }
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
}

TEST_F(RecordingTest, DestructionFinishesActiveSessionAndMissingParentDoesNotCreateOutput) {
    const auto target = directory / "recording.mp4";
    std::string error;
    {
        RecordingSession session(std::make_unique<FakeRecorder>());
        ASSERT_TRUE(session.Start({}, target, error));
    }
    EXPECT_EQ(Read(target), "new-video");
    RecordingSession session(std::make_unique<FakeRecorder>());
    EXPECT_FALSE(session.Start({}, directory / "missing" / "recording.mp4", error));
    EXPECT_FALSE(error.empty());
}
