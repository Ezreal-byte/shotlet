#include "Application.h"
#include "capture/ScreenCapture.h"
#include "core/Canvas.h"
#include "history/HistoryLibrary.h"
#include "platform/ApplicationPlatform.h"
#include "platform/FileService.h"
#include "platform/TransparentWindow.h"
#include "ui/OverlayManager.h"
#include "utils/Logger.h"
#include "utils/EventUtils.h"

namespace shotlet {
    void Application::StartCapture() {
        if (m_recentCaptures) m_recentCaptures->Hide();
        if (!m_embeddedMode && !m_hostCaptureOutput) {
            const auto source = GetForegroundCaptureSource();
            m_captureSourceApplication = source.application;
            m_captureSourceWindow = source.window;
        }
        if (m_settingsWindow && m_settingsWindow->IsVisible()) {
            m_restoreSettingsWindow = true;
            m_settingsWindow->Hide();
        }
        m_historySourceRecordId.clear();
        m_ocrHistoryCandidate.reset();
        if (m_historyWindow && m_historyWindow->IsVisible()) {
            m_restoreHistoryWindow = true;
            m_historyWindow->Hide();
        }
        // 新截图可以在上一张图编码时进入，旧任务完成不能结束新会话。
        m_exitAfterClipboard = false;
        const bool reusePending = m_overlayReclaimTimer.IsPending();
        m_overlayReclaimTimer.Cancel();

        m_captureResultData = {};

        // daemon 模式下热键可在上一张图的 OCR 尚未完成时重新进入截图：
        // 同步收掉旧结果（丢弃），避免覆盖 m_ocrFuture 时隐式阻塞或状态错乱
        if (m_ocrFuture.valid()) {
            Logger::Info("[StartCapture] Discarding pending OCR result");
            (void) m_ocrFuture.get(); // 新截图开始前丢弃上一张图的识别结果
            m_ocrRenderResult = {};
        }

        Logger::BeginCapture();
        m_captureStarted = std::chrono::steady_clock::now();
        Logger::Info("event=capture.started");
        Logger::Info("event=overlay.capture_preparing retained={} reuse_pending={}", m_overlayManager != nullptr,
                     reusePending);

        if (!m_overlayManager) {
            m_overlayManager = std::make_unique<OverlayManager>();
        }
        RefreshTheme(true);
        m_overlayManager->SetThemeChangedCallback([this]() { RefreshTheme(true); });
        if (!m_hostCaptureOutput)
            m_overlayManager->SetExternalEventCallback([this](const SDL_Event &event) { return RouteHistoryEvent(event); });
        if (!m_overlayManager->CreateAll(m_screenCapture.get(), m_language, m_theme)) {
            Logger::Error("Failed to create overlays");
            if (m_daemonMode) {
                ResetOverlayManagerForDaemon();
            } else {
                DestroyOverlayManager();
            }
            return;
        }

        m_capturing = true;
        Logger::Info(
                "event=capture.ready startup_ms={:.1f}",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_captureStarted).count());
        Logger::Debug("StartCapture complete, m_capturing=true");
    }

    void Application::DestroyOverlayManager() {
        m_overlayReclaimTimer.Cancel();
        if (!m_overlayManager)
            return;

        Logger::Debug("[DestroyOverlayManager] begin");
        ResetPlatformCursor();
        WaitForOverlayGpuIdle(m_gpuDevice);
        Logger::Debug("[DestroyOverlayManager] DestroyAll");
        m_overlayManager->DestroyAll();
        Logger::Debug("[DestroyOverlayManager] reset manager");
        m_overlayManager.reset();
        WaitForOverlayGpuIdle(m_gpuDevice);
        Logger::Debug("[DestroyOverlayManager] end");
    }

    void Application::ResetOverlayManagerForDaemon() {
        if (!m_overlayManager)
            return;

        Logger::Debug("[ResetOverlayManagerForDaemon] begin");
        ResetPlatformCursor();
        WaitForOverlayGpuIdle(m_gpuDevice);
        Logger::Debug("[ResetOverlayManagerForDaemon] ResetForReuse");
        m_overlayManager->ResetForReuse();
        WaitForOverlayGpuIdle(m_gpuDevice);
        Logger::Debug("[ResetOverlayManagerForDaemon] end");
        if (SupportsIdleOverlayReclamation() && m_daemonMode && !m_embeddedMode) {
            m_overlayReclaimTimer.Schedule(OverlayReclaimTimer::Clock::now());
            Logger::Info(
                    "event=daemon.overlay_reclaim_scheduled delay_ms={}",
                    std::chrono::duration_cast<std::chrono::milliseconds>(OverlayReclaimTimer::ReuseDelay).count());
        }
    }

    void Application::ReclaimIdleOverlays() {
        if (!SupportsIdleOverlayReclamation() || !m_daemonMode || m_embeddedMode || m_capturing ||
            !m_overlayReclaimTimer.IsDue(OverlayReclaimTimer::Clock::now()))
            return;
        const auto started = OverlayReclaimTimer::Clock::now();
        DestroyOverlayManager();
        Logger::Info("event=daemon.overlays_reclaimed duration_ms={:.1f}",
                     std::chrono::duration<double, std::milli>(OverlayReclaimTimer::Clock::now() - started).count());
    }

    FBORenderer::RenderResult Application::RenderCurrentSelection(const ScreenOverlay::Config *&config, Canvas *&canvas,
                                                                  const char *action) {
        m_outputHistoryCandidate.reset();
        config = nullptr;
        canvas = nullptr;

        if (!m_overlayManager) {
            return {};
        }

        canvas = m_overlayManager->GetResultCanvas();
        config = m_overlayManager->GetResultConfig();
        if (!canvas || !config) {
            return {};
        }

        if (!canvas->HasSelection()) {
            return {};
        }

        canvas->FlushPendingInput();
        auto result = FBORenderer::RenderToOffscreen(m_gpuDevice, m_overlayManager->GetResultSDLWindow(),
                                                     m_overlayManager->GetResultImGuiContext(), canvas,
                                                     canvas->GetSelection(), config->scaleFactor);
        if (m_history && result.data) {
            HistoryImage image{std::shared_ptr<const unsigned char>(result.data, result.data.get()), result.width,
                               result.height};
            m_outputHistoryCandidate = m_history->Prepare(canvas, image, action, m_historySourceRecordId);
            if (m_outputHistoryCandidate)
                m_outputHistoryCandidate->details.title =
                        std::string(Tr(m_language, TextId::HistoryScreenshot)) + " " +
                        HistoryTime(m_outputHistoryCandidate->document.createdAtUnixMs, "%Y-%m-%d %H.%M.%S");
            if (m_outputHistoryCandidate && m_historySourceRecordId.empty()) {
                auto &details = m_outputHistoryCandidate->details;
                if (details.sourceApplication.empty())
                    details.sourceApplication = m_captureSourceApplication;
                if (details.sourceWindow.empty())
                    details.sourceWindow = m_captureSourceWindow;
            }
        }
        return result;
    }

    void Application::CleanupAfterCapture(const char *reason) {
        if (m_overlayManager)
            m_overlayManager->HideAll();
        Logger::Info("event=capture.windows_hidden");
        if (m_daemonMode) {
            ResetOverlayManagerForDaemon();
        } else {
            DestroyOverlayManager();
        }
        if (m_embeddedMode && reason) {
            EventUtils::FlushPendingEvents(reason);
        }
        Logger::Info(
                "event=capture.selection_finished reason={} duration_ms={:.1f}", reason ? reason : "unknown",
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_captureStarted).count());
        m_capturing = false;
    }
} // namespace shotlet
