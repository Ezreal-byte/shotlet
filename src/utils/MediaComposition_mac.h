#pragma once
#import <AVFoundation/AVFoundation.h>
#include "FilePathUtils.h"
#include <filesystem>
#include <string>
#include <vector>

namespace shotlet::MediaComposition {
// 所有轨道使用相同的插入位置，保留系统声音与麦克风之间的同步关系。
inline bool AppendAsset(AVMutableComposition* composition, const std::filesystem::path& path, std::string& error) {
    const auto utf8 = FilePathUtils::ToUtf8(path);
    auto* asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:[NSString stringWithUTF8String:utf8.c_str()]] options:nil];
    __block NSError* failure = nil;
    auto ready = dispatch_semaphore_create(0);
    // 由资产自身加载时长和轨道；不能按录制墙钟截断，否则会裁掉编码器尾帧。
    [composition insertTimeRange:CMTimeRangeMake(kCMTimeZero, asset.duration) ofAsset:asset atTime:composition.duration
        completionHandler:^(NSError* reason) { failure = reason; dispatch_semaphore_signal(ready); }];
    dispatch_semaphore_wait(ready, DISPATCH_TIME_FOREVER);
    if (!failure) return true;
    error = failure.localizedDescription.UTF8String ?: "无法拼接录制片段";
    return false;
}

// 仅在录制后台任务调用；Passthrough 直接重封装 H.264/AAC，不重复压缩画面。
inline bool ConcatenateMp4(const std::vector<std::filesystem::path>& segments, const std::filesystem::path& destination,
                           std::string& error) {
    auto* composition = [AVMutableComposition composition];
    for (const auto& path : segments)
        if (!AppendAsset(composition, path, error)) return false;
    auto* exporter = [[AVAssetExportSession alloc] initWithAsset:composition presetName:AVAssetExportPresetPassthrough];
    if (!exporter) { error = "无法创建 MP4 拼接任务"; return false; }
    const auto utf8 = FilePathUtils::ToUtf8(destination);
    exporter.outputURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:utf8.c_str()]];
    exporter.outputFileType = AVFileTypeMPEG4;
    auto ready = dispatch_semaphore_create(0);
    [exporter exportAsynchronouslyWithCompletionHandler:^{ dispatch_semaphore_signal(ready); }];
    // 一小时的高分辨率视频可能需要较长封装时间，UI 持续泵事件，不使用固定短超时。
    dispatch_semaphore_wait(ready, DISPATCH_TIME_FOREVER);
    if (exporter.status == AVAssetExportSessionStatusCompleted) return true;
    error = exporter.error.localizedDescription.UTF8String ?: "无法保存拼接后的 MP4";
    return false;
}
}
