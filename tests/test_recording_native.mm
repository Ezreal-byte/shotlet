#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <ImageIO/ImageIO.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include "platform/ApplicationPlatform.h"
#include "recording/ScreenRecorder.h"
#include "recording/GifWriter.h"
#include "recording/RecordingVisuals.h"
#include "ui/RecordingWindow.h"
#include "gpu_preview.h"
#include "image_decode_mac.h"
#include "native_events_mac.h"
#include <gtest/gtest.h>
#include <future>
#include <cmath>

using namespace shotlet;
using namespace shotlet::TestUtils;

TEST(RecordingNativeTest, ControlWindowRendersBothThemesAndRoutesStopWithoutClosing) {
    @autoreleasepool {
        ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO)) << SDL_GetError();
        const ScopeExit quit([] { SDL_Quit(); });
        SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
        auto* device = CreateApplicationGpuDevice(false);
        ASSERT_NE(device, nullptr) << SDL_GetError();
        const ScopeExit releaseDevice([&] { DestroyApplicationGpuDevice(device, false); });
        {
            RecordingWindow control(device, Language::ChineseSimplified);
            RecordingOptions options;
            ASSERT_TRUE(PrepareRecordingOptions(int(CGMainDisplayID()), {100, 100, 640, 480}, 2, options));
            ASSERT_TRUE(control.Show(options)) << SDL_GetError();
            EXPECT_EQ(control.GetState(), RecordingWindow::State::Ready);
            for (auto theme : {Theme::Light, Theme::Dark}) {
                control.Render(theme); PumpFor(0.1); control.Render(theme);
                int x = 0, y = 0, width = 0, height = 0;
                ASSERT_TRUE(SDL_GetWindowPosition(control.Window(), &x, &y));
                ASSERT_TRUE(SDL_GetWindowSize(control.Window(), &width, &height));
                EXPECT_EQ(x + width / 2, 420); EXPECT_EQ(y + height / 2, 340);
                std::string error;
                EXPECT_TRUE(shotlet::TestUtils::SaveGpuPreview(device, control.Window(), control.Context(),
                    std::filesystem::temp_directory_path() / (theme == Theme::Dark ? "shotlet-recording-ready-dark.png" : "shotlet-recording-ready-light.png"), error)) << error;
            }
            std::string borderError;
            auto border = CreateRecordingBorder(options, borderError);
            ASSERT_NE(border, nullptr) << borderError; EXPECT_TRUE(border->IsVisible());
            EXPECT_FALSE(control.ConsumeStartRequest());
            SDL_Event begin{}; begin.type = SDL_EVENT_KEY_DOWN; begin.key.windowID = SDL_GetWindowID(control.Window()); begin.key.key = SDLK_RETURN;
            EXPECT_TRUE(control.ProcessEvent(begin)); EXPECT_TRUE(control.ConsumeStartRequest()); EXPECT_FALSE(control.ConsumeStartRequest());
            control.SetState(RecordingWindow::State::Recording);
            for (auto theme : {Theme::Light, Theme::Dark}) {
                control.Render(theme); PumpFor(0.1); control.Render(theme);
                int width = 0, height = 0, pixelWidth = 0, pixelHeight = 0;
                ASSERT_TRUE(SDL_GetWindowSize(control.Window(), &width, &height));
                ASSERT_TRUE(SDL_GetWindowSizeInPixels(control.Window(), &pixelWidth, &pixelHeight));
                EXPECT_EQ(pixelWidth, std::lround(width * SDL_GetWindowPixelDensity(control.Window())));
                EXPECT_EQ(pixelHeight, std::lround(height * SDL_GetWindowPixelDensity(control.Window())));
                EXPECT_EQ(width, 416); EXPECT_EQ(height, 52);
                int x = 0, y = 0;
                ASSERT_TRUE(SDL_GetWindowPosition(control.Window(), &x, &y));
                constexpr int ToolbarGap = 12;
                EXPECT_EQ(x + width, options.region.x + options.region.width);
                EXPECT_EQ(y, options.region.y + options.region.height + ToolbarGap);
                std::string error;
                const auto path = std::filesystem::temp_directory_path() /
                    (theme == Theme::Light ? "shotlet-recording-control-light.png" : "shotlet-recording-control-dark.png");
                EXPECT_TRUE(shotlet::TestUtils::SaveGpuPreview(device, control.Window(), control.Context(), path, error)) << error;
            }
            const auto clickPause = [&] {
                constexpr float PauseCenterX = 62, PauseCenterY = 26;
                const ImGuiUtils::ContextScope context(control.Context());
                auto& io = ImGui::GetIO(); io.ConfigInputTrickleEventQueue = false;
                SDL_Event click{}; click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
                click.button.windowID = SDL_GetWindowID(control.Window()); click.button.button = SDL_BUTTON_LEFT;
                control.ProcessEvent(click);
                io.AddMousePosEvent(PauseCenterX, PauseCenterY); io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
                control.Render(Theme::Dark);
                io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); control.Render(Theme::Dark);
                click.type = SDL_EVENT_MOUSE_BUTTON_UP; control.ProcessEvent(click);
            };
            clickPause(); EXPECT_TRUE(control.ConsumePauseRequest()); EXPECT_FALSE(control.ConsumePauseRequest());
            SDL_Event pause{}; pause.type = SDL_EVENT_KEY_DOWN;
            pause.key.windowID = SDL_GetWindowID(control.Window()); pause.key.key = SDLK_SPACE;
            EXPECT_TRUE(control.ProcessEvent(pause)); EXPECT_TRUE(control.ConsumePauseRequest()); EXPECT_FALSE(control.ConsumePauseRequest());
            control.SetState(RecordingWindow::State::Pausing);
            control.ProcessEvent(pause); EXPECT_FALSE(control.ConsumePauseRequest());
            control.SetState(RecordingWindow::State::Paused);
            control.Render(Theme::Dark);
            std::string pausedError;
            EXPECT_TRUE(SaveGpuPreview(device, control.Window(), control.Context(),
                std::filesystem::temp_directory_path() / "shotlet-recording-control-paused.png", pausedError)) << pausedError;
            clickPause(); EXPECT_TRUE(control.ConsumePauseRequest());
            EXPECT_TRUE(control.ProcessEvent(pause)); EXPECT_TRUE(control.ConsumePauseRequest());
            SDL_Event event{}; event.type = SDL_EVENT_KEY_DOWN;
            event.key.windowID = SDL_GetWindowID(control.Window()); event.key.key = SDLK_ESCAPE;
            EXPECT_TRUE(control.ProcessEvent(event));
            EXPECT_TRUE(control.ConsumeStopRequest()); EXPECT_FALSE(control.ConsumeStopRequest());
            EXPECT_FALSE(control.ConsumeCloseRequest());
            control.SetState(RecordingWindow::State::Saving);
            control.ProcessEvent(event); EXPECT_FALSE(control.ConsumeStopRequest());
            control.SetState(RecordingWindow::State::Saved, "录屏测试.mp4");
            control.Render(Theme::Dark);
            control.ProcessEvent(event); EXPECT_TRUE(control.ConsumeCloseRequest());
        }
    }
}

TEST(RecordingNativeTest, RecordsPlayableH264Mp4WithAudioAndExactRetinaDimensions) {
    @autoreleasepool {
        if (@available(macOS 15.0, *)) {
            if (SDL_getenv("SHOTLET_RECORDING_TEST_REQUEST_PERMISSIONS")) {
                ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO));
                std::string error;
                ASSERT_TRUE(PrepareRecordingPermissions(error)) << error;
            }
            // 此集成测试只使用已经授权的设备；不会在自动测试中弹出麦克风授权。
            if (!CGPreflightScreenCaptureAccess() ||
                [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] != AVAuthorizationStatusAuthorized)
                GTEST_SKIP() << "需要预先授予屏幕录制和麦克风权限";
            if (![AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeAudio]) GTEST_SKIP() << "没有默认麦克风";
            ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO));
            SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0");
            const auto display = CGMainDisplayID();
            const auto bounds = CGDisplayBounds(display);
            CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
            ASSERT_NE(mode, nullptr);
            const double density = CGDisplayModeGetPixelWidth(mode) / bounds.size.width;
            CGDisplayModeRelease(mode);
            RecordingOptions options;
            ASSERT_TRUE(PrepareRecordingOptions(int(display), {bounds.origin.x, bounds.origin.y, 320 / density, 240 / density}, density, options));
            NSString* name = [NSTemporaryDirectory() stringByAppendingFormat:@"shotlet-native-recording-%@.mp4", NSUUID.UUID.UUIDString];
            const std::filesystem::path path(name.UTF8String);
            auto session = std::make_shared<RecordingSession>();
            std::string startError, stopError;
            auto start = std::async(std::launch::async, [session, options, path, &startError] { return session->Start(options, path, startError); });
            ASSERT_TRUE(Await(start)) << startError;
            EXPECT_FALSE(std::filesystem::exists(path));
            PumpFor(1.2);
            auto stop = std::async(std::launch::async, [session, &stopError] { return session->Stop(stopError); });
            ASSERT_TRUE(Await(stop)) << stopError;
            EXPECT_GT(std::filesystem::file_size(path), 0);
            AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:name] options:nil];
            const auto tracks = [&](AVMediaType type) {
                __block NSArray<AVAssetTrack*>* value = nil;
                auto ready = dispatch_semaphore_create(0);
                [asset loadTracksWithMediaType:type completionHandler:^(NSArray<AVAssetTrack*>* result, NSError*) {
                    value = result; dispatch_semaphore_signal(ready);
                }];
                while (dispatch_semaphore_wait(ready, DISPATCH_TIME_NOW) != 0) PumpEvents();
                return value;
            };
            NSArray<AVAssetTrack*>* video = tracks(AVMediaTypeVideo);
            NSArray<AVAssetTrack*>* audio = tracks(AVMediaTypeAudio);
            ASSERT_EQ(video.count, 1);
            // 普通播放器默认只播放一条音轨，系统声音和麦克风必须已经混合。
            ASSERT_EQ(audio.count, 1);
            EXPECT_EQ(video.firstObject.naturalSize.width, options.widthPixels);
            EXPECT_EQ(video.firstObject.naturalSize.height, options.heightPixels);
            EXPECT_GT(CMTimeGetSeconds(asset.duration), 0.8);
            EXPECT_LT(CMTimeGetSeconds(asset.duration), 4.0);
            CMFormatDescriptionRef format = (__bridge CMFormatDescriptionRef)video.firstObject.formatDescriptions.firstObject;
            EXPECT_EQ(CMFormatDescriptionGetMediaSubType(format), uint32_t(kCMVideoCodecType_H264));
            CMFormatDescriptionRef audioFormat = (__bridge CMFormatDescriptionRef)audio.firstObject.formatDescriptions.firstObject;
            EXPECT_EQ(CMFormatDescriptionGetMediaSubType(audioFormat), uint32_t(kAudioFormatMPEG4AAC));
            NSError* error = nil;
            AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset:asset error:&error];
            auto output = [[AVAssetReaderTrackOutput alloc] initWithTrack:video.firstObject
                outputSettings:@{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)}];
            auto audioOutput = [[AVAssetReaderTrackOutput alloc] initWithTrack:audio.firstObject
                outputSettings:@{AVFormatIDKey: @(kAudioFormatLinearPCM)}];
            [reader addOutput:output]; [reader addOutput:audioOutput];
            ASSERT_TRUE([reader startReading]);
            CMSampleBufferRef sample = [output copyNextSampleBuffer];
            ASSERT_NE(sample, nullptr);
            CFRelease(sample);
            CMSampleBufferRef audioSample = [audioOutput copyNextSampleBuffer];
            ASSERT_NE(audioSample, nullptr); EXPECT_GT(CMSampleBufferGetNumSamples(audioSample), 0);
            CFRelease(audioSample); [reader cancelReading];
            [[NSFileManager defaultManager] removeItemAtPath:name error:nil];
            SDL_Quit();
        } else GTEST_SKIP() << "录屏需要 macOS 15+";
    }
}

TEST(RecordingNativeTest, GifFramesRoundTripThroughSystemDecoderAcrossDictionaryResets) {
    @autoreleasepool {
        constexpr int width = 192, height = 128, channels = 4;
        NSString* name = [NSTemporaryDirectory() stringByAppendingFormat:@"shotlet-gif-%@.gif", NSUUID.UUID.UUIDString];
        const ScopeExit cleanup([&] { [[NSFileManager defaultManager] removeItemAtPath:name error:nil]; });
        GifWriter writer; std::string error;
        ASSERT_TRUE(writer.Open(name.UTF8String, width, height, error)) << error;
        std::vector<uint8_t> pixels(size_t(width) * height * channels);
        uint32_t noise = 12345;
        for (auto& value : pixels) { noise = noise * 1664525u + 1013904223u; value = uint8_t(noise >> 24); }
        ASSERT_TRUE(writer.AddBgraFrame(pixels.data(), width * channels, false, 0, error)) << error;
        for (size_t i = 0; i < pixels.size(); i += channels) {
            pixels[i] = 0; pixels[i + 1] = 255; pixels[i + 2] = 0; pixels[i + 3] = 255;
        }
        ASSERT_TRUE(writer.AddBgraFrame(pixels.data(), width * channels, true, 200, error)) << error;
        for (size_t i = 0; i < pixels.size(); i += channels) { pixels[i] = 255; pixels[i + 1] = 0; }
        ASSERT_TRUE(writer.AddBgraFrame(pixels.data(), width * channels, false, 500, error)) << error;
        ASSERT_TRUE(writer.Finish(900, error)) << error;
        CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:name], nullptr);
        ASSERT_NE(source, nullptr);
        const ScopeExit release([&] { CFRelease(source); });
        ASSERT_EQ(CGImageSourceGetCount(source), 3);
        for (size_t index = 0; index < 3; ++index) {
            CGImageRef image = CGImageSourceCreateImageAtIndex(source, index, nullptr);
            ASSERT_NE(image, nullptr) << index;
            EXPECT_EQ(CGImageGetWidth(image), width); EXPECT_EQ(CGImageGetHeight(image), height);
            int decodedWidth = 0, decodedHeight = 0;
            auto decoded = shotlet::TestUtils::DecodeImageFrame(name.UTF8String, index, decodedWidth, decodedHeight, error);
            ASSERT_FALSE(decoded.empty()) << error;
            if (index == 1) { EXPECT_GT(decoded[1], 240); EXPECT_LT(decoded[0], 15); EXPECT_LT(decoded[2], 15); }
            if (index == 2) { EXPECT_GT(decoded[2], 240); EXPECT_LT(decoded[0], 15); EXPECT_LT(decoded[1], 15); }
            CGImageRelease(image);
            NSDictionary* properties = CFBridgingRelease(CGImageSourceCopyPropertiesAtIndex(source, index, nullptr));
            NSDictionary* gif = properties[(id)kCGImagePropertyGIFDictionary];
            const double delay = [gif[(id)kCGImagePropertyGIFUnclampedDelayTime] doubleValue];
            const double expected[] = {0.2, 0.3, 0.4};
            EXPECT_NEAR(delay, expected[index], 0.011);
        }
    }
}

TEST(RecordingNativeTest, GifScreenCaptureKeepsOutlineVisibleUntilRecordingFinishes) {
    @autoreleasepool {
        if (!CGPreflightScreenCaptureAccess()) GTEST_SKIP() << "需要屏幕录制权限";
        const auto display = CGMainDisplayID();
        const auto bounds = CGDisplayBounds(display);
        RecordingOptions options;
        ASSERT_TRUE(PrepareRecordingOptions(int(display), {bounds.origin.x + 100, bounds.origin.y + 100, 320, 240}, 1, options));
        options.format = RecordingFormat::Gif; options.showCursor = false;
        std::string error;
        auto border = CreateRecordingBorder(options, error); ASSERT_NE(border, nullptr) << error;
        NSString* name = [NSTemporaryDirectory() stringByAppendingFormat:@"shotlet-live-gif-%@.gif", NSUUID.UUID.UUIDString];
        const ScopeExit cleanup([&] { [[NSFileManager defaultManager] removeItemAtPath:name error:nil]; });
        auto session = std::make_shared<RecordingSession>();
        const std::filesystem::path path(name.UTF8String);
        auto start = std::async(std::launch::async, [&, session] { return session->Start(options, path, error); });
        ASSERT_TRUE(Await(start)) << error; EXPECT_TRUE(border->IsVisible());
        PumpFor(0.5); EXPECT_TRUE(border->IsVisible());
        auto stop = std::async(std::launch::async, [&, session] { return session->Stop(error); });
        ASSERT_TRUE(Await(stop)) << error; EXPECT_TRUE(border->IsVisible());
        CGImageSourceRef source = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:name], nullptr);
        ASSERT_NE(source, nullptr); EXPECT_GE(CGImageSourceGetCount(source), 1);
        CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0, nullptr);
        ASSERT_NE(image, nullptr); CGImageRelease(image); CFRelease(source);
    }
}

TEST(RecordingNativeTest, CameraOverlayWindowIsCompositedIntoMp4AndGif) {
    @autoreleasepool {
        if (!CGPreflightScreenCaptureAccess()) GTEST_SKIP() << "需要屏幕录制权限";
        ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO));
        const ScopeExit quit([] { SDL_Quit(); });
        const auto display = CGMainDisplayID(); const auto bounds = CGDisplayBounds(display);
        RecordingOptions options;
        ASSERT_TRUE(PrepareRecordingOptions(int(display), {bounds.origin.x, bounds.origin.y, 320, 240}, 1, options));
        options.systemAudio = options.microphone = false; options.camera = true;
        std::string borderError;
        auto border = CreateRecordingBorder(options, borderError); ASSERT_NE(border, nullptr) << borderError;
        options.excludedWindowIds = border->CaptureWindowIds();
        const double screenHeight = CGDisplayBounds(CGMainDisplayID()).size.height;
        NSPanel* background = [[NSPanel alloc] initWithContentRect:NSMakeRect(options.region.x,
            CGYToAppKitBottomY(options.region.y, options.region.height, screenHeight), options.region.width, options.region.height)
            styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        background.releasedWhenClosed = NO; background.hidesOnDeactivate = NO; background.level = NSFloatingWindowLevel;
        background.opaque = YES; background.backgroundColor = [NSColor colorWithSRGBRed:1 green:0 blue:0 alpha:1];
        [background orderFrontRegardless];
        const ScopeExit closeBackground([&] { [background close]; });
        const auto cameraRect = RecordingCameraRect(options, 1);
        const double mainHeight = CGDisplayBounds(CGMainDisplayID()).size.height;
        NSPanel* preview = [[NSPanel alloc] initWithContentRect:NSMakeRect(cameraRect.x,
            CGYToAppKitBottomY(cameraRect.y, cameraRect.height, mainHeight), cameraRect.width, cameraRect.height)
            styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
        preview.releasedWhenClosed = NO; preview.level = NSFloatingWindowLevel; preview.hidesOnDeactivate = NO;
        preview.backgroundColor = [NSColor colorWithSRGBRed:0 green:1 blue:0 alpha:1]; preview.opaque = YES;
        [preview orderFrontRegardless];
        const ScopeExit close([&] { [preview close]; });
        options.cameraWindowId = uint32_t(preview.windowNumber);
        PumpFor(0.1);
        for (const auto format : {RecordingFormat::Mp4, RecordingFormat::Gif}) {
        options.format = format;
        NSString* name = [NSTemporaryDirectory() stringByAppendingFormat:@"shotlet-camera-composite-%@%s", NSUUID.UUID.UUIDString, RecordingExtension(format)];
        const ScopeExit cleanup([&] { [[NSFileManager defaultManager] removeItemAtPath:name error:nil]; });
        auto session = std::make_shared<RecordingSession>();
        std::string error; const std::filesystem::path path(name.UTF8String);
        auto start = std::async(std::launch::async, [&, session] { return session->Start(options, path, error); });
        ASSERT_TRUE(Await(start)) << error; PumpFor(0.5);
        auto stop = std::async(std::launch::async, [&, session] { return session->Stop(error); });
        ASSERT_TRUE(Await(stop)) << error;
        const int cameraX = int(cameraRect.x - options.region.x + cameraRect.width / 2);
        const int cameraY = int(cameraRect.y - options.region.y + cameraRect.height / 2);
        if (format == RecordingFormat::Gif) {
            int width = 0, height = 0;
            auto pixels = shotlet::TestUtils::DecodeImageFrame(path, 0, width, height, error);
            ASSERT_FALSE(pixels.empty()) << error;
            const auto* pixel = pixels.data() + (size_t(cameraY) * width + cameraX) * 4;
            EXPECT_GT(pixel[1], 190); EXPECT_LT(pixel[0], 70); EXPECT_LT(pixel[2], 70);
            const auto* edge = pixels.data() + (size_t(40) * width + 1) * 4;
            EXPECT_GT(edge[0], 190); EXPECT_LT(edge[1], 70); EXPECT_LT(edge[2], 70);
            continue;
        }
        AVURLAsset* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:name] options:nil];
        __block NSArray<AVAssetTrack*>* tracks = nil;
        auto ready = dispatch_semaphore_create(0);
        [asset loadTracksWithMediaType:AVMediaTypeVideo completionHandler:^(NSArray<AVAssetTrack*>* value, NSError*) {
            tracks = value; dispatch_semaphore_signal(ready);
        }];
        while (dispatch_semaphore_wait(ready, DISPATCH_TIME_NOW)) PumpEvents();
        ASSERT_EQ(tracks.count, 1);
        AVAssetReader* reader = [[AVAssetReader alloc] initWithAsset:asset error:nil];
        auto output = [[AVAssetReaderTrackOutput alloc] initWithTrack:tracks.firstObject
            outputSettings:@{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)}];
        [reader addOutput:output]; ASSERT_TRUE([reader startReading]);
        CMSampleBufferRef sample = [output copyNextSampleBuffer]; ASSERT_NE(sample, nullptr);
        const ScopeExit release([&] { CFRelease(sample); [reader cancelReading]; });
        CVPixelBufferRef image = CMSampleBufferGetImageBuffer(sample); ASSERT_NE(image, nullptr);
        ASSERT_EQ(CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly), kCVReturnSuccess);
        const ScopeExit unlock([&] { CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly); });
        const int x = int(cameraRect.x - options.region.x + cameraRect.width / 2);
        const int y = int(cameraRect.y - options.region.y + cameraRect.height / 2);
        const auto* pixel = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(image)) + y * CVPixelBufferGetBytesPerRow(image) + x * 4;
        EXPECT_GT(pixel[1], 190); EXPECT_LT(pixel[0], 70); EXPECT_LT(pixel[2], 70);
        const auto* edge = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(image)) + 40 * CVPixelBufferGetBytesPerRow(image) + 4;
        EXPECT_GT(edge[2], 190); EXPECT_LT(edge[1], 70); EXPECT_LT(edge[0], 70);
        }
    }
}

TEST(RecordingNativeTest, RealCameraPreviewStartsWhenAuthorizedDeviceExists) {
    @autoreleasepool {
        if (![AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo]) GTEST_SKIP() << "没有可用摄像头";
        if (!SDL_getenv("SHOTLET_RECORDING_TEST_REQUEST_PERMISSIONS") &&
            [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo] != AVAuthorizationStatusAuthorized)
            GTEST_SKIP() << "需要摄像头权限";
        RecordingOptions options;
        ASSERT_TRUE(PrepareRecordingOptions(int(CGMainDisplayID()), {200, 200, 640, 480}, 1, options));
        std::string error;
        auto camera = CreateRecordingCamera(options, error);
        if (!camera && [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo] != AVAuthorizationStatusAuthorized)
            GTEST_SKIP() << error;
        ASSERT_NE(camera, nullptr) << error;
        EXPECT_NE(camera->CaptureWindowId(), 0);
        PumpFor(1.0); EXPECT_TRUE(camera->GetError().empty()) << camera->GetError();
    }
}
