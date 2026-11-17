#include <stdexcept>
#include "Application.h"
#include "core/Canvas.h"
#include "platform/OcrWindow.h"
#include "ui/OverlayManager.h"
#include "utils/Logger.h"

namespace shotlet {
    void Application::OnOcrRequested() {
        const ScreenOverlay::Config *config = nullptr;
        Canvas *canvas = nullptr;
        auto result = RenderCurrentSelection(config, canvas, "ocr");

        if (result.data) {
            // 缓存完成后处理所需的上下文（CleanupAfterCapture 会销毁 overlay）
            m_ocrHistoryCandidate = std::move(m_outputHistoryCandidate);
            m_ocrRenderResult = std::move(result);
            m_ocrConfig = *config;
            m_ocrSelection = canvas->GetSelection();
            // 锚点同步缓存：CleanupAfterCapture 后 manager 不可再查询（overlay/编辑器已销毁）
            m_ocrScreenAnchor = m_overlayManager->GetResultSelectionScreenOrigin();
            Logger::Info("[OnOcrRequested] OCR started asynchronously ({}x{})", m_ocrRenderResult.width,
                         m_ocrRenderResult.height);
        } else if (canvas && canvas->HasSelection()) {
            m_exitCode = ExitError;
            Logger::Error("[OnOcrRequested] FBORenderer failed");
        } else if (config) {
            m_exitCode = ExitCancelled;
            Logger::Warning("[OnOcrRequested] No selection in active canvas");
        }

        CleanupAfterCapture("after-ocr-overlay-cleanup");

        if (m_ocrRenderResult.data) {
            // OCR 移出主线程：识别期间主循环保持响应，完成后由
            // ProcessOcrResultIfReady 创建结果窗口/填充嵌入结果
            try {
                m_ocrFuture = OcrService::RecognizeDetailedAsync(m_ocrRenderResult.data.get(), m_ocrRenderResult.width,
                                                                 m_ocrRenderResult.height);
                return;
            } catch (const std::exception &error) {
                Logger::Error("OCR startup failed: {}", error.what());
                m_ocrHistoryCandidate.reset();
                m_ocrRenderResult = {};
                m_exitCode = ExitError;
            }
        }

        if (!m_daemonMode && !m_ocrActive) {
            Quit();
        }
    }

    void Application::ProcessOcrResultIfReady() {
        if (!m_ocrFuture.valid())
            return;
        if (m_ocrFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return;

        OcrResult recognition;
        try {
            recognition = m_ocrFuture.get();
        } catch (const std::exception &e) {
            recognition.error = e.what();
        }
        const std::string &ocrText = recognition.text;
        if (!recognition.success)
            Logger::Error("[OCR] {}", recognition.error);
        Logger::Info("[ProcessOcrResultIfReady] OCR completed, text length={}", ocrText.size());

        if (m_embeddedMode) {
            PopulateCaptureResult(CaptureResultType::Ocr, m_ocrRenderResult, m_ocrConfig, m_ocrSelection,
                                  m_ocrScreenAnchor, ocrText);
            m_exitCode = ExitSuccess;
        } else {
            // m_ocrScreenAnchor 在请求时缓存（此时 overlay/编辑器仍存在可查询）
            const float screenX = m_ocrScreenAnchor.x;
            const float screenY = m_ocrScreenAnchor.y;
            if (OcrWindow::Create(m_ocrRenderResult.data.get(), m_ocrRenderResult.width, m_ocrRenderResult.height,
                                  ocrText, screenX, screenY, m_ocrConfig.scaleFactor, m_language, m_theme)) {
                if (recognition.success && m_history) {
                    if (m_ocrHistoryCandidate && ocrText.size() <= HistoryDetails::MaxOcrTextBytes) {
                        m_ocrHistoryCandidate->details.ocrIndexed = true;
                        m_ocrHistoryCandidate->details.ocrText = ocrText;
                    }
                    m_history->Finish(std::move(m_ocrHistoryCandidate));
                }
                m_ocrActive = true;
                m_hostActionHandled = true;
                m_exitCode = ExitSuccess;
                Logger::Info("[ProcessOcrResultIfReady] OCR result window created, text length={}", ocrText.size());
            } else {
                m_exitCode = ExitError;
                Logger::Error("[ProcessOcrResultIfReady] Failed to create OCR result window");
            }
        }

        m_ocrHistoryCandidate.reset();
        m_ocrRenderResult = {}; // 释放渲染缓冲

        if (!m_daemonMode && !m_ocrActive) {
            Quit();
        }
    }

    void Application::OnOcrWindowClosed() {
        Logger::Info("[OnOcrWindowClosed] OCR window closed");
        OcrWindow::Destroy();
        m_ocrActive = false;

        if (m_daemonMode) {
            Logger::Debug("[OnOcrWindowClosed] Daemon mode: staying in background");
            return;
        }
        Quit();
    }
} // namespace shotlet
