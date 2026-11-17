#include "Application.h"
#include "platform/ClipboardService.h"
#include "utils/Logger.h"

namespace shotlet {
#if defined(SHOTLET_PLATFORM_MACOS)
    namespace {
        struct CopyCompletionContext {
            std::unique_ptr<HistoryService::Candidate> candidate;
            std::shared_ptr<std::atomic<bool>> published = std::make_shared<std::atomic<bool>>(false);
            int version = -1;
            ClipboardCopySource source = ClipboardCopySource::Capture;
            std::shared_ptr<HistoryBudget::Lease> budget;
        };
    } // namespace
#endif

    bool Application::SubmitClipboardCopy(const HistoryImage &image, std::chrono::steady_clock::time_point confirmed,
                                          ClipboardCopySource source, std::shared_ptr<HistoryBudget::Lease> budget) {
        const bool fromHistory = source != ClipboardCopySource::Capture;
#if defined(SHOTLET_PLATFORM_MACOS)
        // macOS 必须等后台 PNG 发布后才能报告成功，空输出沿用错误退出语义。
        if (!image.pixels) {
            if (!fromHistory)
                m_exitCode = ExitError;
            return false;
        }
        ClipboardCopyQueue::Request copy;
        copy.pixels = image.pixels;
        copy.width = image.width;
        copy.height = image.height;
        copy.confirmed = confirmed;
        copy.sequence = ++m_clipboardSequence;

        if (!fromHistory) {
            Logger::Info(
                    "event=capture.output_ready clipboard_copy_id={} width={} height={} output_ms={:.1f}",
                    copy.sequence, copy.width, copy.height,
                    std::chrono::duration<double, std::milli>(ClipboardCopyQueue::Clock::now() - confirmed).count());
        }
        auto context = std::make_shared<CopyCompletionContext>();
        context->source = source;
        context->budget = std::move(budget);
        if (!fromHistory)
            context->candidate = std::move(m_outputHistoryCandidate);
        copy.context = context;
        context->version = DeclareClipboardPng(m_clipboardCopies.Submit(std::move(copy)), context->published);
        m_clipboardVersion = context->version;
        return true;
#else
        (void) confirmed;
        if (!image.pixels)
            return false;
        if (!CopyImageToClipboard(image.pixels.get(), image.width, image.height)) {
            if (!fromHistory) {
                m_exitCode = ExitError;
                Logger::Error("Failed to copy capture to clipboard");
            }
            return false;
        }
        if (fromHistory) {
            NotifyClipboardCopy(source, true);
        } else {
            if (m_history)
                m_history->Finish(std::move(m_outputHistoryCandidate));
            Logger::Info("Capture saved to clipboard ({}x{})", image.width, image.height);
        }
        return true;
#endif
    }

    bool Application::ClipboardCopiesBusy() const {
#if defined(SHOTLET_PLATFORM_MACOS)
        return m_clipboardCopies.Busy();
#else
        return false;
#endif
    }

    void Application::DrainClipboardCopies() {
        while (ClipboardCopiesBusy())
            ProcessClipboardCopies(true);
    }

    void Application::ProcessClipboardCopies(bool wait) {
#if defined(SHOTLET_PLATFORM_MACOS)
        auto result = m_clipboardCopies.TakeCompleted(wait);
        if (!result)
            return;
        auto context = std::static_pointer_cast<CopyCompletionContext>(result->context);
        if (result->superseded) {
            if (m_history && context && context->published->load())
                m_history->Finish(std::move(context->candidate));
            Logger::Info("event=capture.clipboard_superseded clipboard_copy_id={}", result->sequence);
            return;
        }
        const auto started = std::chrono::steady_clock::now();
        const int version = context ? context->version : m_clipboardVersion;
        const bool replaced = version >= 0 && !IsClipboardPngCurrent(version);
        const bool copied = result->png && (replaced || (version >= 0 && PublishClipboardPng(result->png, version)));
        if (m_history && context && (context->published->load() || (copied && !replaced)))
            m_history->Finish(std::move(context->candidate));
        if (replaced)
            Logger::Info("event=capture.clipboard_replaced clipboard_copy_id={}", result->sequence);
        if (m_exitAfterClipboard)
            m_exitCode = copied ? ExitSuccess : ExitError;
        Logger::Info("event=capture.clipboard_finished clipboard_copy_id={} success={} encode_ms={:.1f} "
                     "publish_ms={:.1f} total_ms={:.1f}",
                     result->sequence, copied, result->encodeMs,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count(),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - result->confirmed)
                             .count());
        if (copied && !replaced)
            Logger::Info("Capture saved to clipboard ({}x{})", result->width, result->height);
        else if (!copied)
            Logger::Error("Failed to copy capture to clipboard: {}", result->error);
        if (context && context->source != ClipboardCopySource::Capture && !replaced)
            NotifyClipboardCopy(context->source, copied);
        if (m_exitAfterClipboard && !m_clipboardCopies.Busy() && !m_capturing) {
            m_exitAfterClipboard = false;
            Quit();
        }
#else
        (void) wait;
#endif
    }

    void Application::NotifyClipboardCopy(ClipboardCopySource source, bool copied) {
        if (source == ClipboardCopySource::History) {
            if (!m_historyWindow || !m_historyWindow->IsVisible()) return;
            if (copied) m_historyWindow->ShowToast(Tr(m_language, TextId::HistoryCopyDone));
            else m_historyWindow->SetError(Tr(m_language, TextId::HistoryCopyFailed));
            return;
        }
        if (!m_running || m_capturing || !m_recentCaptures) return;
        if (source == ClipboardCopySource::Recent) {
            if (m_recentCaptures->IsVisible()) m_recentCaptures->SetCopyResult(copied);
        } else if (source == ClipboardCopySource::Latest) {
            m_recentCaptures->ShowFeedback(m_systemTray.GetAnchor(),
                Tr(m_language, copied ? TextId::RecentCapturesCopied : TextId::HistoryCopyFailed), !copied);
        }
    }

} // namespace shotlet
