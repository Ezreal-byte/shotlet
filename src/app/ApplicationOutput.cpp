#include "Application.h"
#include "core/Canvas.h"
#include "history/HistoryLibrary.h"
#include "platform/FileService.h"
#include "platform/PinWindow.h"
#include "platform/TransparentWindow.h"
#include "ui/OverlayManager.h"
#include "utils/Logger.h"

namespace shotlet {
    namespace {
        constexpr int RgbaBytesPerPixel = 4;
        constexpr float OcrWindowWidthRatio = 1.5f;

        const char *CaptureResultTypeName(CaptureResultType type) {
            switch (type) {
                case CaptureResultType::Pin:
                    return "pin";
                case CaptureResultType::Ocr:
                    return "ocr";
                case CaptureResultType::None:
                    return "none";
            }
            return "unknown";
        }
    } // namespace

    void Application::PopulateCaptureResult(CaptureResultType type, const FBORenderer::RenderResult &renderResult,
                                            const ScreenOverlay::Config &config, const ImRect &selection,
                                            const ImVec2 &screenPos, const std::string &text) {
        const int rowPitch = renderResult.width * RgbaBytesPerPixel;
        const auto dataSize = static_cast<std::size_t>(rowPitch) * static_cast<std::size_t>(renderResult.height);
        const float selectionWidth = selection.GetWidth();
        const float selectionHeight = selection.GetHeight();
        // screenPos 由调用方经 OverlayManager::GetResultSelectionScreenOrigin 取得：
        // overlay 路径=窗口原点+选区；编辑器路径=图片坐标经编辑器视图变换（windowX+sel 会失配）
        const float screenX = screenPos.x;
        const float screenY = screenPos.y;

        auto &target = m_captureResultData;
        target = {};
        target.type = type;
        target.imageWidth = renderResult.width;
        target.imageHeight = renderResult.height;
        target.rowPitch = rowPitch;
        target.selectionX = screenX;
        target.selectionY = screenY;
        target.selectionWidth = selectionWidth;
        target.selectionHeight = selectionHeight;
        target.windowX = screenX;
        target.windowY = screenY;
        target.windowWidth = type == CaptureResultType::Ocr ? selectionWidth * OcrWindowWidthRatio : selectionWidth;
        target.windowHeight = selectionHeight;
        target.scaleFactor = config.scaleFactor;
        target.text = text;
        target.rgba.assign(renderResult.data.get(), renderResult.data.get() + dataSize);

        Logger::Debug("[CaptureResult][populate] type={}, image={}x{}, rowPitch={}, selection=({:.1f},{:.1f} "
                      "{:.1f}x{:.1f}), screen=({:.1f},{:.1f}), window={}x{}, scaleFactor={:.3f}, textLength={}",
                      CaptureResultTypeName(type), target.imageWidth, target.imageHeight, target.rowPitch,
                      selection.Min.x, selection.Min.y, selectionWidth, selectionHeight, screenX, screenY,
                      target.windowWidth, target.windowHeight, target.scaleFactor, target.text.size());
    }

    HistoryImage Application::RenderClipboardImage() {
#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)
        const auto *overlay = m_overlayManager->GetActiveOverlay();
        const auto scrollImage = overlay ? overlay->GetConfirmedScrollImage() : nullptr;
        if (scrollImage) {
            HistoryImage image{std::shared_ptr<const unsigned char>(scrollImage, scrollImage->rgba.data()),
                               scrollImage->width, scrollImage->height};
            if (m_history) {
                m_outputHistoryCandidate = m_history->Prepare(nullptr, image, "copy");
                if (m_outputHistoryCandidate) {
                    auto &details = m_outputHistoryCandidate->details;
                    details.sourceApplication = m_captureSourceApplication;
                    details.sourceWindow = m_captureSourceWindow;
                    details.title =
                            std::string(Tr(m_language, TextId::HistoryScreenshot)) + " " +
                            HistoryTime(m_outputHistoryCandidate->document.createdAtUnixMs, "%Y-%m-%d %H.%M.%S");
                }
            }
            return image;
        }
#endif
        const ScreenOverlay::Config *config = nullptr;
        Canvas *canvas = nullptr;
        auto result = RenderCurrentSelection(config, canvas, "copy");
        if (result.data)
            return {std::shared_ptr<const unsigned char>(result.data, result.data.get()), result.width, result.height};
        if (canvas && canvas->HasSelection()) {
            m_exitCode = ExitError;
            Logger::Error("FBORenderer failed to produce output");
        } else if (config) {
            Logger::Warning("No selection in active canvas");
        }
        return {};
    }

    void Application::OnCaptureFinished(bool success) {
#if defined(SHOTLET_PLATFORM_LINUX)
        // The standalone Linux preview completes through PNG export. Host/SDK
        // mode keeps returning pixels and never acquires the system clipboard.
        if (success && !m_hostCaptureOutput && !m_embeddedMode) {
            OnSaveRequested();
            return;
        }
#endif
        const auto confirmed = std::chrono::steady_clock::now();
        Logger::Debug("[OnCaptureFinished] begin success={}", success);
        m_exitCode = success ? ExitSuccess : ExitCancelled;
        bool copying = false;
        if (success) {
            auto image = RenderClipboardImage();
            // 在普通截图和长截图共用的出口交付本次像素，不能从剪贴板推断结果。
            if (m_hostCaptureOutput) {
                // 宿主只接收图片，不改系统剪贴板，也不启动后台剪贴板编码。
                if (!m_hostCaptureOutput(image)) m_exitCode = ExitError;
            } else {
                copying = SubmitClipboardCopy(image, confirmed);
            }
        }

        CleanupAfterCapture("after-capture-finished-overlay-cleanup");
        Logger::Info("event=capture.confirm_ui_finished confirmed={} duration_ms={:.1f}", success,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - confirmed).count());
        m_outputHistoryCandidate.reset();
        Logger::Debug("[OnCaptureFinished] cleanup complete");

        if (m_daemonMode) {
            Logger::Info("event=daemon.idle reason=capture-finished");
            return;
        }
        // 同步复制的平台可直接退出；后台编码仍持有像素，须等主线程发布完成。
        if (copying && ClipboardCopiesBusy()) {
            m_exitAfterClipboard = true;
            return;
        }
        Quit();
    }

    void Application::OnPinRequested() {
        const ScreenOverlay::Config *config = nullptr;
        Canvas *canvas = nullptr;
        auto result = RenderCurrentSelection(config, canvas, "pin");
        if (result.data) {
            const auto &overlayConfig = *config;
            ImRect sel = canvas->GetSelection();
            // 锚点经 manager 路由：overlay 路径=窗口原点+选区；编辑器路径=选区在
            // 编辑器窗口中的实际显示位置（直接 windowX+图片坐标会弹到屏幕外）
            const ImVec2 screenPos = m_overlayManager->GetResultSelectionScreenOrigin();
            const float screenX = screenPos.x;
            const float screenY = screenPos.y;

            if (m_embeddedMode) {
                PopulateCaptureResult(CaptureResultType::Pin, result, overlayConfig, sel, screenPos);
                m_exitCode = ExitSuccess;
                Logger::Info("[OnPinRequested] Pin result returned to host ({}x{}) at screen ({:.0f}, {:.0f})",
                             result.width, result.height, screenX, screenY);
            } else {
                bool pinCreated = PinWindow::Create(result.data.get(), result.width, result.height, screenX, screenY,
                                                    overlayConfig.scaleFactor, m_language, m_theme);
                if (pinCreated) {
                    if (m_history)
                        m_history->Finish(std::move(m_outputHistoryCandidate));
                    m_pinned = true;
                    m_hostActionHandled = true;
                    m_exitCode = ExitSuccess;
                    Logger::Info("[OnPinRequested] Pin result window created ({}x{}) at screen ({:.0f}, {:.0f})",
                                 result.width, result.height, screenX, screenY);
                } else {
                    m_exitCode = ExitError;
                    Logger::Error("[OnPinRequested] Failed to create pin result window");
                }
            }
        } else if (canvas && canvas->HasSelection()) {
            m_exitCode = ExitError;
            Logger::Error("[OnPinRequested] FBORenderer failed to produce output");
        } else if (config) {
            m_exitCode = ExitCancelled;
            Logger::Warning("[OnPinRequested] No selection in active canvas");
        }

        CleanupAfterCapture("after-pin-overlay-cleanup");
        m_outputHistoryCandidate.reset();

        if (!m_daemonMode && !m_pinned) {
            Quit();
        }
    }

    void Application::OnPinWindowClosed() {
        Logger::Info("[OnPinWindowClosed] Pin window closed");
        PinWindow::Destroy();
        m_pinned = false;

        if (m_daemonMode) {
            Logger::Debug("[OnPinWindowClosed] Daemon mode: staying in background");
            return;
        }
        Quit();
    }

    void Application::OnSaveRequested() {
        const ScreenOverlay::Config *config = nullptr;
        Canvas *canvas = nullptr;
        auto renderResult = RenderCurrentSelection(config, canvas, "save");

        CleanupAfterCapture(nullptr);

        ActivateAppForSaveDialog();

        if (renderResult.data) {
            std::string path =
                    SaveFileWithDialog(renderResult.data.get(), renderResult.width, renderResult.height, m_language);
            if (!path.empty()) {
                m_exitCode = ExitSuccess;
                m_hostActionHandled = true;
                if (m_history)
                    m_history->Finish(std::move(m_outputHistoryCandidate));
                Logger::Info("[OnSaveRequested] event=image.saved");
            } else {
                m_exitCode = ExitCancelled;
                Logger::Info("[OnSaveRequested] Save cancelled by user");
            }
        } else {
            Logger::Error("[OnSaveRequested] FBORenderer failed to produce output");
            m_exitCode = ExitError;
        }

        m_outputHistoryCandidate.reset();
        if (m_daemonMode) {
            RestoreActivationPolicy();
            Logger::Debug("[OnSaveRequested] Daemon mode: staying in background");
            return;
        }

        Quit();
    }
} // namespace shotlet
