#pragma once
#include "i18n/Localization.h"
#include "recording/ScreenRecorder.h"
#include "ui/Theme.h"
#include <filesystem>
#include <memory>
#include <string>

namespace shotlet {
namespace RecordingPlaybackLayout {
inline constexpr int PreferredWidthLogical = 900, PreferredHeightLogical = 600;
inline constexpr int MinimumWidthLogical = 420, MinimumHeightLogical = 280;
inline constexpr int FooterHeightLogical = 48, PaddingLogical = 12;
inline constexpr int ButtonWidthLogical = 140, ButtonHeightLogical = 28;
inline constexpr double WorkAreaFraction = 0.8;
}

// 只在主线程创建、更新和销毁；最小化仍算打开，关闭或析构时停止播放并释放文件。
class RecordingPlaybackWindow {
public:
    virtual ~RecordingPlaybackWindow() = default;
    virtual bool IsOpen() const = 0;
    virtual void Update(Theme theme) = 0;
};

std::unique_ptr<RecordingPlaybackWindow> CreateRecordingPlaybackWindow(
    const std::filesystem::path& path, const RecordingOptions& options,
    Language language, Theme theme, std::string& error);
}
