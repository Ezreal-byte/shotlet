#pragma once
#include "utils/AtomicFile.h"
#include "utils/CoordinateUtils.h"
#include <filesystem>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

struct SDL_Window;
namespace shotlet {
inline constexpr int RecordingFramesPerSecond = 30;
inline constexpr int GifFramesPerSecond = 10;
inline constexpr auto MaximumRecordingDuration = std::chrono::hours(1);
enum class RecordingFormat { Mp4, Gif };
inline const char* RecordingExtension(RecordingFormat format) { return format == RecordingFormat::Gif ? ".gif" : ".mp4"; }
struct RecordingOptions {
    int displayId = -1;
    // macOS 使用全局逻辑点，Windows 使用全局实际像素，与选区窗口保持一致。
    RectD region;
    int widthPixels = 0, heightPixels = 0;
    RecordingFormat format = RecordingFormat::Mp4;
    bool systemAudio = true, microphone = true, showCursor = true, camera = false;
    // macOS 使用 WindowServer ID，不能替换成 SDL 窗口 ID；只排除明确列出的录制界面。
    uint32_t cameraWindowId = 0;
    std::vector<uint32_t> excludedWindowIds;
    bool HasAudio() const { return format == RecordingFormat::Mp4 && (systemAudio || microphone); }
};

// 对齐 H.264 的偶数像素边界，同时修正源区域，避免奇数选区被拉伸。
bool PrepareRecordingOptions(int displayId, RectD region, double pixelDensity, RecordingOptions& result);

class IScreenRecorder {
public:
    virtual ~IScreenRecorder() = default;
    // 启停和暂停/继续可等待原生回调，只能串行在后台任务调用；GetError 可由界面线程轮询。
    virtual bool Start(const RecordingOptions& options, const std::filesystem::path& path, std::string& error) = 0;
    virtual bool Stop(std::string& error) = 0;
    virtual bool SetPaused(bool paused, std::string& error) = 0;
    virtual std::string GetError() const = 0;
    virtual bool HasStopped() const { return false; }
};
std::unique_ptr<IScreenRecorder> CreateScreenRecorder(RecordingFormat format = RecordingFormat::Mp4);
void ExcludeRecordingControlWindow(SDL_Window* window);
uint32_t RecordingControlWindowId(SDL_Window* window);
// 当前系统是否支持屏幕录制；macOS 需要 15+，用于隐藏录屏入口。
bool IsScreenRecordingSupported();
// 在主线程检查能力并申请麦克风权限，避免后台任务触发无法交互的系统弹窗。
bool PrepareRecordingPermissions(std::string& error, bool microphone = true);

class RecordingSession {
public:
    explicit RecordingSession(std::unique_ptr<IScreenRecorder> recorder = {});
    ~RecordingSession();
    bool Start(const RecordingOptions& options, const std::filesystem::path& target, std::string& error);
    bool Stop(std::string& error);
    bool SetPaused(bool paused, std::string& error);
    bool IsPaused() const { return m_paused; }
    std::string GetError() const { return m_recorder ? m_recorder->GetError() : std::string(); }
    bool HasStopped() const { return m_recorder && m_recorder->HasStopped(); }
    bool IsActive() const { return m_started; }
private:
    std::unique_ptr<IScreenRecorder> m_recorder;
    AtomicFile m_file;
    bool m_started = false;
    bool m_nativeRecorder = true;
    bool m_paused = false;
};
}
