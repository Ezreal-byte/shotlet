#pragma once
#include "native_events_mac.h"
#include "utils/FilePathUtils.h"
#include "utils/ScopeExit.h"
#import <AVFoundation/AVFoundation.h>
#include <array>
#include <vector>

namespace shotlet::TestUtils {
struct DecodedVideoFrame {
    double timeSeconds = 0;
    std::array<unsigned char, 3> centerRgb{};
};

inline NSArray<AVAssetTrack*>* LoadAssetTracks(AVAsset* asset, AVMediaType type, std::string& error) {
    __block NSArray<AVAssetTrack*>* tracks = nil;
    __block NSError* failure = nil;
    auto ready = dispatch_semaphore_create(0);
    [asset loadTracksWithMediaType:type completionHandler:^(NSArray<AVAssetTrack*>* value, NSError* reason) {
        tracks = value; failure = reason; dispatch_semaphore_signal(ready);
    }];
    if (!PumpUntil([&] { return dispatch_semaphore_wait(ready, DISPATCH_TIME_NOW) == 0; })) {
        error = "加载媒体轨道超时"; return nil;
    }
    if (failure) error = failure.localizedDescription.UTF8String;
    return tracks;
}

// 解码整条视频轨并逐帧释放样本；资产总时长可能来自音轨，不能证明画面持续有效。
inline std::vector<DecodedVideoFrame> DecodeVideoFrames(const std::filesystem::path& path, std::string& error) {
    const auto utf8 = FilePathUtils::ToUtf8(path);
    auto* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:utf8.c_str()]] options:nil];
    const auto tracks = LoadAssetTracks(asset, AVMediaTypeVideo, error);
    if (!tracks.count) { if (error.empty()) error = "没有视频轨"; return {}; }
    NSError* failure = nil;
    auto* reader = [[AVAssetReader alloc] initWithAsset:asset error:&failure];
    auto* output = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:tracks.firstObject
        outputSettings:@{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)}];
    if (!reader || ![reader canAddOutput:output]) { error = "无法创建视频解码器"; return {}; }
    [reader addOutput:output];
    if (![reader startReading]) { error = reader.error.localizedDescription.UTF8String; return {}; }
    std::vector<DecodedVideoFrame> result;
    while (true) {
        @autoreleasepool {
            CMSampleBufferRef sample = [output copyNextSampleBuffer];
            if (!sample) break;
            const ScopeExit release([&] { CFRelease(sample); });
            CVPixelBufferRef image = CMSampleBufferGetImageBuffer(sample);
            if (!image || CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) {
                error = "视频帧解码失败"; return {};
            }
            const ScopeExit unlock([&] { CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly); });
            constexpr int PixelChannels = 4;
            const auto* center = static_cast<const unsigned char*>(CVPixelBufferGetBaseAddress(image)) +
                CVPixelBufferGetHeight(image) / 2 * CVPixelBufferGetBytesPerRow(image) + CVPixelBufferGetWidth(image) / 2 * PixelChannels;
            result.push_back({CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(sample)), {center[2], center[1], center[0]}});
        }
    }
    if (reader.status != AVAssetReaderStatusCompleted) { error = reader.error.localizedDescription.UTF8String ?: "视频解码未完成"; return {}; }
    return result;
}
}
