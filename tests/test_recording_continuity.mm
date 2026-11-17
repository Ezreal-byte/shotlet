#import <AppKit/AppKit.h>
#import <AVKit/AVKit.h>
#include "platform/RecordingPlaybackWindow.h"
#include "recording/ScreenRecorder.h"
#include "recording/RecordingVisuals.h"
#include "video_decode_mac.h"
#include "window_preview_mac.h"
#include "image_decode_mac.h"
#include "utils/Logger.h"
#include <gtest/gtest.h>
#include <future>

using namespace shotlet;
using namespace shotlet::TestUtils;
namespace {
constexpr int RegionWidthLogical = 480, RegionHeightLogical = 320;
constexpr int ColorUpdates = 120, MinimumVideoFrames = 60;
constexpr double ColorStepSeconds = 0.05, RecordingSeconds = ColorUpdates * ColorStepSeconds;
constexpr double DurationToleranceSeconds = 0.5;
constexpr int ReplayCount = 3, MinimumDisplayedBlue = 160, MaximumDisplayedRed = 90;
constexpr double PlaybackTimeoutSlackSeconds = 3, PlaybackEndToleranceSeconds = 0.1;

void VerifyRepeatedPlayback(const std::filesystem::path& path, const RecordingOptions& options) {
    std::string error;
    auto preview = CreateRecordingPlaybackWindow(path, options, Language::ChineseSimplified, Theme::Dark, error);
    ASSERT_NE(preview, nullptr) << error;
    auto* window = FindNativeWindow(@"shotlet.recording.playback"); ASSERT_NE(window, nil);
    auto* view = FindNativeView<AVPlayerView>(window, AVPlayerView.class); ASSERT_NE(view, nil);
    ASSERT_TRUE(PumpUntil([&] { return view.player.currentItem.status == AVPlayerItemStatusReadyToPlay; }));
    const double durationSeconds = CMTimeGetSeconds(view.player.currentItem.duration);
    ASSERT_GE(durationSeconds, RecordingSeconds - DurationToleranceSeconds);
    for (int replay = 0; replay < ReplayCount; ++replay) {
        SCOPED_TRACE(replay);
        if (replay) {
            auto ready = dispatch_semaphore_create(0);
            [view.player seekToTime:kCMTimeZero toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero completionHandler:^(BOOL finished) {
                if (finished) dispatch_semaphore_signal(ready);
            }];
            ASSERT_TRUE(PumpUntil([&] { return dispatch_semaphore_wait(ready, DISPATCH_TIME_NOW) == 0; }));
            [view.player play];
        }
        ASSERT_TRUE(PumpUntil([&] {
            return view.player.rate == 0 && CMTimeGetSeconds(view.player.currentTime) >= durationSeconds - PlaybackEndToleranceSeconds;
        }, durationSeconds + PlaybackTimeoutSlackSeconds));
        EXPECT_EQ(view.player.currentItem.error, nil);
        const auto imagePath = std::filesystem::temp_directory_path() / ("shotlet-continuity-replay-" + std::to_string(replay) + ".png");
        ASSERT_TRUE(SaveNativeWindowPreview(window, imagePath, error)) << error;
        int width = 0, height = 0;
        const auto pixels = DecodeImageFrame(imagePath, 0, width, height, error);
        ASSERT_FALSE(pixels.empty()) << error;
        constexpr int PixelChannels = 4;
        const auto center = (size_t(height / 2) * width + width / 2) * PixelChannels;
        // 在每次播放结束后读取系统合成画面，检查最后的蓝色帧仍可见，而非只看播放器时间。
        EXPECT_LT(pixels[center], MaximumDisplayedRed);
        EXPECT_GT(pixels[center + 2], MinimumDisplayedBlue);
    }
}

void VerifyContinuousRecording(bool withCamera, bool withAudio = true) {
    SCOPED_TRACE(withCamera ? "摄像头开启" : "摄像头关闭");
    SCOPED_TRACE(withAudio ? "系统声音与麦克风开启" : "音频关闭");
    if (!CGPreflightScreenCaptureAccess()) GTEST_SKIP() << "需要屏幕录制权限";
    if (withAudio && [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] != AVAuthorizationStatusAuthorized)
        GTEST_SKIP() << "需要麦克风权限";
    ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO));
    const auto display = CGMainDisplayID();
    const auto bounds = CGDisplayBounds(display);
    RecordingOptions options;
    CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
    ASSERT_NE(mode, nullptr);
    const double pixelDensity = CGDisplayModeGetPixelWidth(mode) / bounds.size.width;
    CGDisplayModeRelease(mode);
    ASSERT_TRUE(PrepareRecordingOptions(display, {bounds.origin.x + 100, bounds.origin.y + 100, RegionWidthLogical, RegionHeightLogical}, pixelDensity, options));
    options.systemAudio = options.microphone = withAudio;
    options.showCursor = false;
    auto* panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(options.region.x,
        CGYToAppKitBottomY(options.region.y, options.region.height, bounds.size.height), options.region.width, options.region.height)
        styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    panel.releasedWhenClosed = NO; panel.hidesOnDeactivate = NO; panel.level = NSFloatingWindowLevel;
    panel.backgroundColor = [NSColor colorWithSRGBRed:1 green:0 blue:0 alpha:1];
    [panel orderFrontRegardless];
    const ScopeExit close([&] { [panel close]; });
    std::string error;
    std::unique_ptr<RecordingCamera> camera;
    if (withCamera) {
        if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo] != AVAuthorizationStatusAuthorized ||
            ![AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo]) GTEST_SKIP() << "需要已授权的摄像头";
        camera = CreateRecordingCamera(options, error); ASSERT_NE(camera, nullptr) << error;
        options.camera = true; options.cameraWindowId = camera->CaptureWindowId();
        PumpFor(0.2);
    }
    NSString* filename = [NSTemporaryDirectory() stringByAppendingFormat:@"shotlet-continuity-%@.mp4", NSUUID.UUID.UUIDString];
    const std::filesystem::path path(filename.UTF8String);
    const ScopeExit remove([&] { std::error_code ignored; std::filesystem::remove(path, ignored); });
    RecordingSession session;
    auto start = std::async(std::launch::async, [&] { return session.Start(options, path, error); });
    ASSERT_TRUE(Await(start)) << error;
    for (int step = 0; step < ColorUpdates; ++step) {
        const double fraction = double(step) / (ColorUpdates - 1);
        panel.backgroundColor = [NSColor colorWithSRGBRed:1 - fraction green:0 blue:fraction alpha:1];
        PumpFor(ColorStepSeconds);
    }
    auto stop = std::async(std::launch::async, [&] { return session.Stop(error); });
    ASSERT_TRUE(Await(stop)) << error;
    camera.reset();
    [panel orderOut:nil];
    const auto frames = DecodeVideoFrames(path, error);
    ASSERT_FALSE(frames.empty()) << error;
    EXPECT_GT(frames.size(), MinimumVideoFrames);
    EXPECT_GT(frames.back().timeSeconds - frames.front().timeSeconds, RecordingSeconds - DurationToleranceSeconds);
    // 首尾颜色差异验证后半段确实继续采集，不能用重复首帧凑够视频时长。
    EXPECT_GT(frames.front().centerRgb[0], frames.front().centerRgb[2]);
    EXPECT_GT(frames.back().centerRgb[2], frames.back().centerRgb[0]);
    Logger::Info("event=test.recording_continuity camera={} audio={} decoded_frames={} video_seconds={}",
        withCamera, withAudio, frames.size(), frames.back().timeSeconds - frames.front().timeSeconds);
    if (!withCamera && withAudio) VerifyRepeatedPlayback(path, options);
}
}

TEST(RecordingContinuityTest, Mp4ContinuesCapturingToEndWithoutCamera) { @autoreleasepool { VerifyContinuousRecording(false); } }
TEST(RecordingContinuityTest, Mp4ContinuesCapturingToEndWithCamera) { @autoreleasepool { VerifyContinuousRecording(true); } }
TEST(RecordingContinuityTest, SilentMp4ContinuesCapturingToEnd) { @autoreleasepool { VerifyContinuousRecording(false, false); } }
