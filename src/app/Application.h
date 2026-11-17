#pragma once
#include <imgui.h>
#include <memory>
#include <functional>
#include <utility>
#include <future>
#include <string>
#include <chrono>
#include <vector>
#include "output/FBORenderer.h"
#include "ui/ScreenOverlay.h"
#include "platform/SystemTray.h"
#include "platform/HotkeyBinding.h"
#include "OverlayReclaimTimer.h"
#include "ClipboardCopyQueue.h"
#include "history/HistoryService.h"
#include "platform/OcrService.h"
#include "ui/HistoryWindow.h"
#include "ui/SettingsWindow.h"
#include "ui/RecentCapturesWindow.h"
#include "ui/RecordingWindow.h"
#include "recording/ScreenRecorder.h"
#include "recording/RecordingVisuals.h"
#include "platform/RecordingPlaybackWindow.h"

struct SDL_GPUDevice;

namespace shotlet {
    struct CaptureImage;
    class Canvas;
    class HistoryWindow;
    class IScreenCapture;
    class OverlayManager;
    class ScreenOverlay;

    enum ExitCode {
        ExitSuccess = 0,    // 用户完成截图，并且复制/保存/贴图/OCR成功
        ExitError = 1,      // 初始化失败、渲染失败、保存失败等程序错误
        ExitCancelled = 2   // 用户取消、Esc、关闭截图
    };

    enum class CaptureResultType {
        None,
        Pin,
        Ocr,
    };

    enum class ClipboardCopySource { Capture, History, Recent, Latest };

    struct CaptureResultData {
        CaptureResultType type = CaptureResultType::None;
        int imageWidth = 0;
        int imageHeight = 0;
        int rowPitch = 0;
        float selectionX = 0.0f;
        float selectionY = 0.0f;
        float selectionWidth = 0.0f;
        float selectionHeight = 0.0f;
        float windowX = 0.0f;
        float windowY = 0.0f;
        float windowWidth = 0.0f;
        float windowHeight = 0.0f;
        float scaleFactor = 1.0f;
        std::string text;
        std::vector<unsigned char> rgba;
    };

    class Application {
    public:
        explicit Application(bool embeddedMode = false);
        ~Application();
        Application(const Application &) = delete;
        Application &operator=(const Application &) = delete;

        // argv 使用 UTF-8；Windows 独立入口负责转换，Node 参数本身为 UTF-8。
        bool Initialize(int argc, char **argv);
        int Run();
        void RequestCapture();
        void Quit();

        IScreenCapture *GetScreenCapture() { return m_screenCapture.get(); }
        SDL_GPUDevice *GetGPUDevice() { return m_gpuDevice; }
        const CaptureResultData& GetCaptureResultData() const { return m_captureResultData; }
        // 截图宿主在初始化前注册输出回调；默认关闭，既有应用和 Node 行为不变。
        // true 接受本次实际图片，false 拒绝输出；有回调时不记录独立截图历史。
        void SetHostCaptureOutput(std::function<bool(const HistoryImage&)> output) {
            m_hostCaptureOutput = std::move(output);
        }
        // 宿主模式下 OCR、贴图、保存或录制已在 SDK 内部完成时为 true，此时没有图片可返回。
        bool HostActionHandled() const { return m_hostActionHandled; }
        static Application *Instance() { return s_instance; }
        bool IsDaemonMode() const { return m_daemonMode; }
        // swapchain drawable 数量（macOS CAMetalLayer maximumDrawableCount，仅 2 或 3）
        int GetGpuBufferCount() const { return m_gpuBufferCount; }

    private:
        friend class RecentCapturesWindowTest;
        friend class RecordingPlaybackTest;
        friend class RecordingPauseFlowTest;
        friend class ScreenOverlayLifecycleTest;
        static constexpr int DoubleGpuBufferCount = 2;
        static constexpr int TripleGpuBufferCount = 3;

        // 日志参数先于平台初始化生效，初始化失败也能保留诊断信息。
        bool InitializeLogging(int argc, char** argv);
        bool InitializePreferences(int argc, char** argv);
        void ParseGpuBufferArguments(int argc, char** argv);
        bool InitializeScreenCapture();
        void InitializeHistory();
        bool InitializeDaemonMode();
        // 主线程解析偏好并同步所有界面；事件可强制立即查询。
        void RefreshTheme(bool force = false);
        // 打开设置窗口；保存成功后同步运行中的全局快捷键及磁盘偏好。
        void ShowSettings();
        void ProcessSettings();
        bool SaveSettings(const Preferences& preferences, std::string& error);
        bool SaveLogDirectory(const std::filesystem::path& directory, std::string& error);
        void PollHotkeys();
        void RefreshStorageInfo();
        bool InitializeSDL();
        bool CreateGPUDevice();
        void Shutdown();
        void StartCapture();
        void DestroyOverlayManager();
        void ResetOverlayManagerForDaemon();
        // macOS daemon 空闲到期后销毁截图窗口，保留贴图/OCR 和共享设备。
        void ReclaimIdleOverlays();
        // 单帧先分发系统输入，再绘制录制控制窗口，避免悬停反馈落后一帧。
        void ProcessFrame();
        // 推进截图帧，并分发用户选择的完成动作。
        void ProcessCaptureFrame();
        // 贴图或 OCR 结果窗口存在时，继续处理窗口事件。
        void ProcessAuxiliaryWindowsFrame();
        // 空闲时处理系统事件，并轮询后台 OCR 结果。
        void ProcessIdleFrame();
        // 轮询后台 OCR 识别结果：完成则在主线程创建结果窗口/填充嵌入结果。
        void ProcessOcrResultIfReady();
        // macOS 后台编码后在主线程发布；其他平台在提交时同步复制。
        bool SubmitClipboardCopy(const HistoryImage& image,
                                 std::chrono::steady_clock::time_point confirmed,
                                 ClipboardCopySource source = ClipboardCopySource::Capture,
                                 std::shared_ptr<HistoryBudget::Lease> budget = {});
        // 复制反馈按入口分发，托盘使用时不要求完整历史窗口存在。
        void NotifyClipboardCopy(ClipboardCopySource source, bool copied);
        void ProcessClipboardCopies(bool wait = false);
        // CLI/addon 返回前排空队列，历史服务和窗口必须保持有效。
        void DrainClipboardCopies();
        bool ClipboardCopiesBusy() const;
        // 渲染编辑器或活动截图窗口中的选区，并向输出动作提供上下文。
        FBORenderer::RenderResult RenderCurrentSelection(const ScreenOverlay::Config*& config, Canvas*& canvas, const char* action);
        HistoryImage RenderClipboardImage();
        // Pin/OCR 共用坐标换算，确保宿主得到屏幕逻辑坐标和实际像素尺寸。
        void PopulateCaptureResult(CaptureResultType type,
                                   const FBORenderer::RenderResult& renderResult,
                                   const ScreenOverlay::Config& config,
                                   const ImRect& selection,
                                   const ImVec2& screenPos,
                                   const std::string& text = {});
        void ProcessHistory();
        bool RouteHistoryEvent(const SDL_Event& event);
        void OpenHistoryEditor(const std::string& id);
        void OpenHistoryDocument(std::shared_ptr<const CaptureDocument> document);
        bool HandleHistoryLibraryRequest(const HistoryWindow::Request& request, std::string& error);
        void HandleHistoryOutputRequest(const HistoryWindow::Request& request, std::string& error);
        void PollHistoryLibrary();
        // 快速历史的后台读取、原图复制与界面动作统一在主线程收尾。
        void ProcessRecentCaptures();
        bool EnsureRecentCaptures();
        void RequestRecentCopy(const std::string& id, bool latest = false);
        void RequestRecentImage(const std::string& id, RecentCapturesWindow::Action action, bool latest = false);
        bool HandleRecentImageAction(const HistoryImage& image);
        void PollRecentCopy();
        // 完成截图后清理窗口，嵌入模式同时清空待处理的 SDL 事件。
        void CleanupAfterCapture(const char* reason);
        void OnCaptureFinished(bool success);
        void OnPinRequested();
        void OnPinWindowClosed();
        void OnOcrRequested();
        void OnOcrWindowClosed();
        void OnSaveRequested();
        // 录制任务借用会话，窗口与事件始终留在主线程；退出前等待原生 MP4 收尾。
        void OnRecordRequested();
        void StartRecording();
        void SyncRecordingCamera();
        void ProcessRecording();
        void StopRecording();
        void ToggleRecordingPause();
        void CompleteRecording(bool saved, const std::string& error);
        void ShowRecordingPlayback();
        bool RecordingBusy() const;
        bool RouteRecordingEvent(const SDL_Event& event);
        void FinishRecordingForShutdown();

        static Application *s_instance;

        std::unique_ptr<IScreenCapture> m_screenCapture;
        std::unique_ptr<OverlayManager> m_overlayManager;
        OverlayReclaimTimer m_overlayReclaimTimer;
        bool m_running = false;
        bool m_capturing = false;
        struct RecordingTaskResult { bool success = false; std::string error; };
        std::unique_ptr<RecordingWindow> m_recordingWindow;
        std::unique_ptr<RecordingPlaybackWindow> m_recordingPlayback;
        std::unique_ptr<RecordingBorder> m_recordingBorder;
        std::unique_ptr<RecordingCamera> m_recordingCamera;
        RecordingOptions m_recordingOptions;
        std::shared_ptr<RecordingSession> m_recordingSession;
        std::future<RecordingTaskResult> m_recordingTask;
        std::filesystem::path m_recordingPath;
        bool m_recordingStopPending = false;
        std::chrono::steady_clock::time_point m_captureStarted{};
        bool m_pinned = false;
        bool m_ocrActive = false;
        // OCR 异步识别状态：pending 期间主循环保持响应并轮询完成。
        // overlay 在 OCR 启动前已被销毁，完成后所需上下文全部缓存在这里。
        std::future<OcrResult> m_ocrFuture;
        FBORenderer::RenderResult m_ocrRenderResult;
        ScreenOverlay::Config m_ocrConfig;
        ImRect m_ocrSelection = ImRect(0, 0, 0, 0);
        // OCR 请求时缓存的结果锚点（CleanupAfterCapture 后 manager 已不可查询）
        ImVec2 m_ocrScreenAnchor = ImVec2(-1, -1);
        std::unique_ptr<HistoryService> m_history;
        std::unique_ptr<HistoryWindow> m_historyWindow;
        std::unique_ptr<RecentCapturesWindow> m_recentCaptures;
        std::unique_ptr<HistoryService::Candidate> m_outputHistoryCandidate, m_ocrHistoryCandidate;
        std::string m_historySourceRecordId;
        std::string m_captureSourceApplication, m_captureSourceWindow, m_historyCopyTextPending;
        struct HistoryOperationResult {
            HistoryImage image;
            std::string error;
            std::shared_ptr<HistoryBudget::Lease> budget;
        };
        std::future<HistoryOperationResult> m_historyOperation;
        std::future<HistoryOperationResult> m_recentCopy;
        bool m_recentCopyIsLatest = false;
        RecentCapturesWindow::Action m_recentImageAction = RecentCapturesWindow::Action::Copy;
        bool m_historyPending = false, m_restoreHistoryWindow = false;
        bool m_daemonMode = false;
        SystemTray m_systemTray;
        int m_exitCode = ExitCancelled;
        CaptureResultData m_captureResultData;
        bool m_hotkeyRegistered = false;
        bool m_historyHotkeyRegistered = false;
        bool m_copyLatestHotkeyRegistered = false;
        Preferences m_preferences;
        std::unique_ptr<SettingsWindow> m_settingsWindow;
        bool m_settingsPending = false;
        bool m_restoreSettingsWindow = false;
        std::chrono::steady_clock::time_point m_nextStorageCheck{};
        bool m_embeddedMode = false;
        std::function<bool(const HistoryImage&)> m_hostCaptureOutput;
        bool m_hostActionHandled = false;
        bool m_sdlInitializedByThisInstance = false;
        Language m_language = Language::English;
        ThemePreference m_themePreference = ThemePreference::Auto;
        Theme m_theme = Theme::Dark;
        std::optional<Theme> m_trayTheme;
        std::chrono::steady_clock::time_point m_nextThemeCheck{};
        int m_gpuBufferCount = DoubleGpuBufferCount;
        SDL_GPUDevice* m_gpuDevice = nullptr;
        bool m_exitAfterClipboard = false;
#if defined(SHOTLET_PLATFORM_MACOS)
        ClipboardCopyQueue m_clipboardCopies;
        uint64_t m_clipboardSequence = 0;
        int m_clipboardVersion = -1;
#endif
    };
} // namespace shotlet
