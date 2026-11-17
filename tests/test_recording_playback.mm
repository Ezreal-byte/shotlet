#import <AVKit/AVKit.h>
#include "app/Application.h"
#include "platform/ApplicationPlatform.h"
#include "recording/GifWriter.h"
#include "native_events_mac.h"
#include "window_preview_mac.h"
#include "image_decode_mac.h"
#include "utils/ScopeExit.h"
#include <gtest/gtest.h>
#include <vector>

namespace {
constexpr int VideoWidthPixels = 160, VideoHeightPixels = 120, FrameCount = 20, FramesPerSecond = 10;
constexpr int PixelChannels = 4;
bool WriteTestVideo(const std::filesystem::path& path, std::string& error) {
    NSError* failure = nil;
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
    auto* writer = [[AVAssetWriter alloc] initWithURL:url fileType:AVFileTypeMPEG4 error:&failure];
    if (!writer) { error = failure.localizedDescription.UTF8String; return false; }
    auto* input = [AVAssetWriterInput assetWriterInputWithMediaType:AVMediaTypeVideo outputSettings:@{
        AVVideoCodecKey: AVVideoCodecTypeH264, AVVideoWidthKey: @(VideoWidthPixels), AVVideoHeightKey: @(VideoHeightPixels)}];
    auto* pixels = [AVAssetWriterInputPixelBufferAdaptor assetWriterInputPixelBufferAdaptorWithAssetWriterInput:input
        sourcePixelBufferAttributes:@{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA),
            (id)kCVPixelBufferWidthKey: @(VideoWidthPixels), (id)kCVPixelBufferHeightKey: @(VideoHeightPixels)}];
    [writer addInput:input];
    if (![writer startWriting]) { error = writer.error.localizedDescription.UTF8String; return false; }
    const shotlet::ScopeExit cancel([&] { if (writer.status == AVAssetWriterStatusWriting) [writer cancelWriting]; });
    [writer startSessionAtSourceTime:kCMTimeZero];
    for (int frame = 0; frame < FrameCount; ++frame) {
        if (!shotlet::TestUtils::PumpUntil([&] { return input.readyForMoreMediaData || writer.status == AVAssetWriterStatusFailed; })) return false;
        CVPixelBufferRef buffer = nullptr;
        if (CVPixelBufferPoolCreatePixelBuffer(nullptr, pixels.pixelBufferPool, &buffer) != kCVReturnSuccess) return false;
        CVPixelBufferLockBaseAddress(buffer, 0);
        auto* base = static_cast<unsigned char*>(CVPixelBufferGetBaseAddress(buffer));
        const auto stride = CVPixelBufferGetBytesPerRow(buffer);
        for (int y = 0; y < VideoHeightPixels; ++y) for (int x = 0; x < VideoWidthPixels; ++x) {
            auto* pixel = base + y * stride + x * PixelChannels;
            pixel[0] = 220; pixel[1] = static_cast<unsigned char>(frame * 10); pixel[2] = 20; pixel[3] = 255;
        }
        CVPixelBufferUnlockBaseAddress(buffer, 0);
        const bool added = [pixels appendPixelBuffer:buffer withPresentationTime:CMTimeMake(frame, FramesPerSecond)];
        CVPixelBufferRelease(buffer);
        if (!added) { error = writer.error.localizedDescription.UTF8String; return false; }
    }
    [input markAsFinished];
    auto done = dispatch_semaphore_create(0);
    [writer finishWritingWithCompletionHandler:^{ dispatch_semaphore_signal(done); }];
    if (!shotlet::TestUtils::PumpUntil([&] { return dispatch_semaphore_wait(done, DISPATCH_TIME_NOW) == 0; })) return false;
    return writer.status == AVAssetWriterStatusCompleted;
}

NSWindow* PlaybackWindow() {
    return shotlet::TestUtils::FindNativeWindow(@"shotlet.recording.playback");
}
void SaveWindowPreview(NSWindow* window, NSString* filename) {
    if (!CGPreflightScreenCaptureAccess()) return;
    const std::filesystem::path path([NSTemporaryDirectory() stringByAppendingPathComponent:filename].UTF8String);
    std::string error;
    ASSERT_TRUE(shotlet::TestUtils::SaveNativeWindowPreview(window, path, error)) << error;
    int width = 0, height = 0;
    const auto pixels = shotlet::TestUtils::DecodeImageFrame(path, 0, width, height, error);
    ASSERT_FALSE(pixels.empty()) << error;
    const auto center = (size_t(height / 2) * width + width / 2) * PixelChannels;
    // 两个素材的中央均为有色像素，防止只检查播放时间而漏掉视频层黑屏。
    EXPECT_GT(std::max({pixels[center], pixels[center + 1], pixels[center + 2]}), 120);
}
}

namespace shotlet {
class RecordingPlaybackTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO)) << SDL_GetError();
        SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
        m_app = std::make_unique<Application>();
        m_app->m_gpuDevice = CreateApplicationGpuDevice(false);
        ASSERT_NE(m_app->m_gpuDevice, nullptr) << SDL_GetError();
        m_app->m_running = true; m_app->m_daemonMode = true; m_app->m_language = Language::ChineseSimplified;
        const auto display = CGMainDisplayID();
        const auto bounds = CGDisplayBounds(display);
        ASSERT_TRUE(PrepareRecordingOptions(display, {bounds.origin.x + 50, bounds.origin.y + 50, 320, 240}, 1, m_app->m_recordingOptions));
        m_app->m_recordingWindow = std::make_unique<RecordingWindow>(m_app->m_gpuDevice, m_app->m_language);
        ASSERT_TRUE(m_app->m_recordingWindow->Show(m_app->m_recordingOptions));
        m_app->m_recordingWindow->SetState(RecordingWindow::State::Saving);
        const auto directory = std::filesystem::temp_directory_path() / NSUUID.UUID.UUIDString.UTF8String;
        std::filesystem::create_directory(directory);
        m_directory = directory;
    }
    void TearDown() override {
        m_app.reset();
        std::error_code error; std::filesystem::remove_all(m_directory, error);
        TestUtils::PumpFor(0.05);
    }
    void UseFile(const std::filesystem::path& path, RecordingFormat format) {
        m_app->m_recordingPath = path; m_app->m_recordingOptions.format = format;
    }
    void Complete(bool success = true) { m_app->CompleteRecording(success, success ? "" : "模拟保存失败"); }
    void Process() { m_app->ProcessRecording(); }
    bool HasPreview() const { return bool(m_app->m_recordingPlayback); }
    bool HasControls() const { return bool(m_app->m_recordingWindow); }
    bool IsRunning() const { return m_app->m_running; }
    RecordingWindow::State State() const { return m_app->m_recordingWindow->GetState(); }
    void ShuttingDown() { m_app->m_running = false; }
    void SingleShot() { m_app->m_daemonMode = false; }
    std::unique_ptr<Application> m_app;
    std::filesystem::path m_directory;
};

TEST_F(RecordingPlaybackTest, SavedMp4AutomaticallyPlaysAndSupportsPauseSeekAndMinimize) {
    const auto path = m_directory / "录屏 播放.mp4";
    std::string error;
    ASSERT_TRUE(WriteTestVideo(path, error)) << error;
    UseFile(path, RecordingFormat::Mp4);
    Complete();
    ASSERT_TRUE(HasPreview()); EXPECT_FALSE(HasControls());
    auto* window = PlaybackWindow(); ASSERT_NE(window, nil);
    auto* view = TestUtils::FindNativeView<AVPlayerView>(window, AVPlayerView.class); ASSERT_NE(view, nil);
    ASSERT_TRUE(TestUtils::PumpUntil([&] { return view.player.currentItem.status == AVPlayerItemStatusReadyToPlay; }));
    ASSERT_TRUE(TestUtils::PumpUntil([&] { return CMTimeGetSeconds(view.player.currentTime) > 0.1; }));
    EXPECT_EQ(view.controlsStyle, AVPlayerViewControlsStyleFloating); EXPECT_TRUE(view.showsFullScreenToggleButton);
    [view.player pause];
    const double paused = CMTimeGetSeconds(view.player.currentTime);
    TestUtils::PumpFor(0.15); EXPECT_NEAR(CMTimeGetSeconds(view.player.currentTime), paused, 0.03);
    auto sought = dispatch_semaphore_create(0);
    [view.player seekToTime:CMTimeMake(1, 1) toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero
        completionHandler:^(BOOL finished) { if (finished) dispatch_semaphore_signal(sought); }];
    ASSERT_TRUE(TestUtils::PumpUntil([&] { return dispatch_semaphore_wait(sought, DISPATCH_TIME_NOW) == 0; }));
    EXPECT_NEAR(CMTimeGetSeconds(view.player.currentTime), 1, 0.05);
    [window miniaturize:nil]; TestUtils::PumpFor(0.15); Process();
    EXPECT_TRUE(HasPreview()); EXPECT_TRUE(IsRunning());
    [window deminiaturize:nil]; TestUtils::PumpFor(0.15);
    SaveWindowPreview(window, @"shotlet-recording-playback-mp4.png");
    AVPlayer* player = view.player;
    [window performClose:nil]; Process();
    EXPECT_FALSE(HasPreview()); EXPECT_TRUE(IsRunning()); EXPECT_EQ(player.currentItem, nil);
    EXPECT_TRUE(std::filesystem::exists(path));
}

TEST_F(RecordingPlaybackTest, SavedGifAnimatesAndSingleShotExitsOnlyAfterPreviewCloses) {
    const auto path = m_directory / "录屏 动画.gif";
    GifWriter writer;
    std::string error;
    constexpr int SidePixels = 80;
    std::vector<unsigned char> pixels(SidePixels * SidePixels * PixelChannels, 0);
    ASSERT_TRUE(writer.Open(path, SidePixels, SidePixels, error)) << error;
    for (size_t index = 0; index < pixels.size(); index += PixelChannels) pixels[index + 1] = 255;
    ASSERT_TRUE(writer.AddBgraFrame(pixels.data(), SidePixels * PixelChannels, false, 0, error));
    for (size_t index = 0; index < pixels.size(); index += PixelChannels) { pixels[index + 1] = 0; pixels[index + 2] = 255; }
    ASSERT_TRUE(writer.AddBgraFrame(pixels.data(), SidePixels * PixelChannels, false, 200, error));
    ASSERT_TRUE(writer.Finish(400, error));
    UseFile(path, RecordingFormat::Gif); SingleShot(); Complete();
    ASSERT_TRUE(HasPreview()); EXPECT_FALSE(HasControls()); EXPECT_TRUE(IsRunning());
    auto* window = PlaybackWindow(); ASSERT_NE(window, nil);
    auto* view = TestUtils::FindNativeView<NSImageView>(window, NSImageView.class); ASSERT_NE(view, nil);
    EXPECT_TRUE(view.animates); EXPECT_TRUE(view.image.isValid);
    auto* representation = static_cast<NSBitmapImageRep*>(view.image.representations.firstObject);
    const auto initialFrame = [[representation valueForProperty:NSImageCurrentFrame] integerValue];
    EXPECT_TRUE(TestUtils::PumpUntil([&] { return [[representation valueForProperty:NSImageCurrentFrame] integerValue] != initialFrame; }));
    SaveWindowPreview(window, @"shotlet-recording-playback-gif.png");
    [window performClose:nil]; Process();
    EXPECT_FALSE(HasPreview()); EXPECT_FALSE(IsRunning()); EXPECT_EQ(view.image, nil);
    EXPECT_TRUE(std::filesystem::exists(path));
}

TEST_F(RecordingPlaybackTest, MissingFileSaveFailureAndShutdownDoNotOpenPreview) {
    UseFile(m_directory / "missing.mp4", RecordingFormat::Mp4); Complete();
    EXPECT_FALSE(HasPreview()); ASSERT_TRUE(HasControls()); EXPECT_EQ(State(), RecordingWindow::State::Saved);
    Complete(false);
    EXPECT_FALSE(HasPreview()); EXPECT_EQ(State(), RecordingWindow::State::Failed);
    ShuttingDown(); Complete();
    EXPECT_FALSE(HasPreview()); EXPECT_EQ(State(), RecordingWindow::State::Saved);
}
}
