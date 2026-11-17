#include "Application.h"
#include <SDL3/SDL.h>
#include "capture/ScreenCapture.h"
#include "platform/ApplicationPlatform.h"
#include "platform/HotkeyService.h"
#include "platform/OcrWindow.h"
#include "platform/PinWindow.h"
#include "platform/PlatformMainContext.h"
#include "ui/OverlayManager.h"
#include "utils/Logger.h"
#include "utils/WindowUtils.h"

namespace shotlet {
    namespace {
        constexpr Uint32 IdleFrameDelayMs = 16;
    }

    Application *Application::s_instance = nullptr;

    Application::Application(bool embeddedMode) : m_embeddedMode(embeddedMode) { s_instance = this; }

    Application::~Application() {
        Shutdown();
        s_instance = nullptr;
    }

    int Application::Run() {
        m_running = true;

        while (m_running) {
            // SDL 内部的局部 pool 不涵盖本帧所有 Cocoa 调用。
            // 排空 subviews 等临时容器，避免其间接持有已释放的 Metal layer。
            PlatformAutoreleasePool pool;
            ProcessFrame();
        }

        FinishRecordingForShutdown();
        if (m_history)
            m_history->StopAccepting();
        DrainClipboardCopies();
        if (m_history) {
            m_history->Drain();
            ProcessHistory();
        }
        Logger::Debug("[Run] Exiting, returning {}", m_exitCode);
        return m_exitCode;
    }

    void Application::ProcessFrame() {
        ProcessClipboardCopies();
        if (!m_running) return;
        // 宿主不轮询历史、设置、托盘与热键，仅推进截图及其 OCR、贴图、保存、录制窗口。
        if (!m_hostCaptureOutput && !RecordingBusy()) {
            ProcessRecentCaptures();
            ProcessHistory();
            ProcessSettings();
        }
        RefreshTheme();
        if (!m_hostCaptureOutput) HotkeyService::SetSuspended(m_settingsWindow && m_settingsWindow->IsRecording());
        const bool captureFrame = m_capturing && m_overlayManager;
        if (captureFrame) ProcessCaptureFrame();
        else if (m_pinned || m_ocrActive) ProcessAuxiliaryWindowsFrame();
        else ProcessIdleFrame();
        if (!m_running) return;
        // 同一轮事件泵收到的鼠标位置和按钮请求必须进入本帧，而不是等下一帧。
        ProcessRecording();
        if (!m_running) return;
        PollHotkeys();
        if (m_systemTray.IsQuitRequested()) {
            m_exitCode = ExitSuccess;
            Quit();
        }
        if (!m_running) return;
        ReclaimIdleOverlays();
        // 可绘制的录制窗口已由 GPU 垂直同步限速，不能再叠加空闲休眠。
        // 隐藏、最小化或切换到原生播放窗口后，恢复空闲限速，避免主线程空转。
        const bool recordingControlsDrawable = m_recordingWindow && WindowUtils::IsDrawable(m_recordingWindow->Window());
        if (!captureFrame && !recordingControlsDrawable) SDL_Delay(IdleFrameDelayMs);
    }

    void Application::ProcessCaptureFrame() {
        m_overlayManager->ProcessFrame();
        // 菜单退出优先于同一帧的截图完成动作，避免退出时再打开结果窗口。
        if (m_systemTray.IsQuitRequested())
            return;

        if (m_overlayManager->IsFinished()) {
            if (m_overlayManager->IsPinRequested()) {
                OnPinRequested();
            } else if (m_overlayManager->IsRecordRequested()) {
                OnRecordRequested();
            } else if (m_overlayManager->IsOcrRequested()) {
                OnOcrRequested();
            } else if (m_overlayManager->IsSaveRequested()) {
                OnSaveRequested();
            } else {
                OnCaptureFinished(true);
            }
        } else if (m_overlayManager->IsCancelled()) {
            OnCaptureFinished(false);
        }

        DelayAfterCaptureFrame();
    }

    void Application::ProcessAuxiliaryWindowsFrame() {
        PinWindow::ProcessEvents();
        OcrWindow::ProcessEvents();
        if (m_pinned && !PinWindow::IsVisible()) {
            OnPinWindowClosed();
        }
        if (m_ocrActive && !OcrWindow::IsVisible()) {
            OnOcrWindowClosed();
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (RouteRecordingEvent(event) || RouteHistoryEvent(event))
                continue;
            if (event.type == SDL_EVENT_SYSTEM_THEME_CHANGED) {
                RefreshTheme(true);
                continue;
            }
            if (event.type == SDL_EVENT_QUIT) {
                if (m_pinned)
                    PinWindow::Destroy();
                if (m_ocrActive)
                    OcrWindow::Destroy();
                Quit();
            }
        }
    }

    void Application::ProcessIdleFrame() {
        // OCR pending 期间必然走此分支（m_capturing/m_pinned/m_ocrActive 均为 false），
        // 每帧廉价轮询一次识别结果
        ProcessOcrResultIfReady();

        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (RouteRecordingEvent(event) || RouteHistoryEvent(event))
                continue;
            if (event.type == SDL_EVENT_SYSTEM_THEME_CHANGED) {
                RefreshTheme(true);
                continue;
            }
            if (event.type == SDL_EVENT_QUIT) {
                Quit();
            }
        }
    }

    void Application::RequestCapture() {
        if (RecordingBusy()) { StopRecording(); return; }
        if (m_recordingWindow || m_recordingPlayback) return;
        if (m_capturing) {
            Logger::Warning("Capture already in progress");
            return;
        }

        StartCapture();
    }

    void Application::Quit() {
        Logger::Info("event=app.quit_requested daemon={} capturing={} tray_quit_requested={}", m_daemonMode,
                     m_capturing, m_systemTray.IsQuitRequested());
        m_running = false;
    }

    void Application::Shutdown() {
        PlatformAutoreleasePool pool;
        FinishRecordingForShutdown();
        // 先移除托盘入口，后续等待 OCR 时不再接受菜单操作。
        if (m_history)
            m_history->StopAccepting();
        m_systemTray.Destroy();
        m_settingsWindow.reset();
        m_recentCaptures.reset();
        if (m_recentCopy.valid()) {
            try { (void)m_recentCopy.get(); } catch (...) {}
        }
        HotkeyService::SetSuspended(false);
        DrainClipboardCopies();
        // 显式等待 pending OCR（std::async future 析构本就会 join，
        // 这里先收掉并记日志，避免退出时无解释地卡住）
        if (m_ocrFuture.valid()) {
            Logger::Info("[Shutdown] Waiting for pending OCR to finish");
            (void) m_ocrFuture.get(); // 退出时仅等待任务完成，丢弃识别结果
            m_ocrRenderResult = {};
        }
        if (m_ocrActive) {
            OcrWindow::Destroy();
            m_ocrActive = false;
        }
        if (m_pinned) {
            PinWindow::Destroy();
            m_pinned = false;
        }
        m_ocrHistoryCandidate.reset();
        m_outputHistoryCandidate.reset();
        if (m_history)
            m_history->Drain();
        if (m_historyOperation.valid()) {
            try {
                (void) m_historyOperation.get();
            } catch (...) {
            }
        }
        m_historyWindow.reset();
        m_history.reset();
        DestroyOverlayManager();
        if (m_hotkeyRegistered) {
            HotkeyService::Unregister();
            m_hotkeyRegistered = false;
        }
        HotkeyService::Unregister(HotkeyService::Action::History);
        m_historyHotkeyRegistered = false;
        HotkeyService::Unregister(HotkeyService::Action::CopyLatest);
        m_copyLatestHotkeyRegistered = false;
        m_screenCapture.reset();
        DestroyApplicationGpuDevice(m_gpuDevice, m_embeddedMode);
        // 嵌入式模式（Node addon）不退出 SDL video 子系统。
        // SDL_QuitSubSystem 会拆除 SDL 的 Cocoa 事件钩子，但 NSApp 事件队列中
        // 可能残留 OCR 等原生 NSWindow 的生命周期事件；第二次调用 SDL_Init
        // 重新挂钩后，SDL_PollEvent 处理这些残留事件时访问已失效的内部状态，导致崩溃。保持 SDL 初始化可以避免这个问题。
        if (m_sdlInitializedByThisInstance && !m_embeddedMode) {
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
            m_sdlInitializedByThisInstance = false;
        }
        Logger::Info("Shotlet shutdown complete");
        Logger::DetachSDLLogging();
        Logger::Flush();
    }
} // namespace shotlet
