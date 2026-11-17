#include "Application.h"
#include "platform/ApplicationPlatform.h"
#include "platform/FileService.h"
#include "platform/PinWindow.h"
#include "platform/TransparentWindow.h"
#include "utils/Logger.h"

namespace shotlet {
namespace {
constexpr size_t kCopyDecodeBufferCopies = 4;
constexpr float kRecentPinPosition = 80;
constexpr float kRecentPinScale = 1;
}

bool Application::EnsureRecentCaptures() {
    if (!m_history) return false;
    if (!m_recentCaptures) m_recentCaptures = std::make_unique<RecentCapturesWindow>(m_gpuDevice, *m_history, m_language, m_theme);
    return true;
}

void Application::RequestRecentCopy(const std::string& id, bool latest) {
    RequestRecentImage(id, RecentCapturesWindow::Action::Copy, latest);
}

void Application::RequestRecentImage(const std::string& id, RecentCapturesWindow::Action action, bool latest) {
    if (m_recentCopy.valid() || !EnsureRecentCaptures()) return;
    m_recentCopyIsLatest = latest;
    m_recentImageAction = action;
    if (latest) m_recentCaptures->ShowFeedback(m_systemTray.GetAnchor(), Tr(m_language, TextId::RecentCapturesCopying));
    const auto language = m_language;
    m_recentCopy = std::async(std::launch::async, [service = m_history.get(), id, latest, language] {
        HistoryOperationResult result;
        std::string recordId = id;
        if (latest) {
            const auto records = service->Store().List();
            const auto sorted = QueryHistory(records, {}, HistoryNow());
            if (sorted.empty()) { result.error = Tr(language, TextId::RecentCapturesEmpty); return result; }
            recordId = records[sorted.front()].id;
        }
        const auto record = service->Store().Get(recordId, result.error);
        if (!record || record->details.deletedAtUnixMs) {
            result.error = Tr(language, TextId::RecentCapturesMissing); return result;
        }
        const auto bytes = DocumentPixelBytes(record->width, record->height);
        if (!bytes) { result.error = Tr(language, TextId::HistoryCopyFailed); return result; }
        auto budget = service->ReserveTask(*bytes * kCopyDecodeBufferCopies);
        if (!budget) { result.error = Tr(language, TextId::HistoryCopyFailed); return result; }
        result.budget = std::make_shared<HistoryBudget::Lease>(std::move(budget));
        result.image = service->Store().LoadResult(recordId, false, result.error, true);
        if (!result.image) result.error = Tr(language, TextId::RecentCapturesMissing);
        return result;
    });
}

bool Application::HandleRecentImageAction(const HistoryImage& image) {
    using Action = RecentCapturesWindow::Action;
    if (m_recentImageAction == Action::Save) {
        ActivateAppForSaveDialog();
        SaveFileWithDialog(image.pixels.get(), image.width, image.height, m_language);
        if (m_daemonMode) RestoreActivationPolicy();
        return true;
    }
    if (m_recentImageAction == Action::Pin) {
        if (PinWindow::Create(image.pixels.get(), image.width, image.height,
            kRecentPinPosition, kRecentPinPosition, kRecentPinScale, m_language, m_theme)) m_pinned = true;
        else m_recentCaptures->ShowFeedback(m_systemTray.GetAnchor(), Tr(m_language, TextId::RecentCapturesUnavailable), true);
        return true;
    }
    return false;
}

void Application::PollRecentCopy() {
    // 原图读取完成后若已进入截图，另存/贴图延后收尾，避免结果窗口覆盖截图界面。
    if (m_capturing && m_recentImageAction != RecentCapturesWindow::Action::Copy) return;
    if (!m_recentCopy.valid() || m_recentCopy.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    try {
        auto result = m_recentCopy.get();
        if (!result.error.empty() || !result.image) {
            if (m_recentCaptures && !m_capturing) {
                if (m_recentCopyIsLatest || !m_recentCaptures->IsVisible()) m_recentCaptures->ShowFeedback(m_systemTray.GetAnchor(), result.error, true);
                else m_recentCaptures->SetError(result.error);
            }
            return;
        }
        if (HandleRecentImageAction(result.image)) return;
        const auto source = m_recentCopyIsLatest ? ClipboardCopySource::Latest : ClipboardCopySource::Recent;
        if (!SubmitClipboardCopy(result.image, std::chrono::steady_clock::now(), source, std::move(result.budget)))
            NotifyClipboardCopy(source, false);
    } catch (const std::exception& error) {
        Logger::Error("[RecentCaptures] Copy failed: {}", error.what());
        NotifyClipboardCopy(m_recentCopyIsLatest ? ClipboardCopySource::Latest : ClipboardCopySource::Recent, false);
    }
}

void Application::ProcessRecentCaptures() {
    PollRecentCopy();
    if (!m_running || m_capturing) return;
    if (m_systemTray.ConsumeCopyLatestRequest()) RequestRecentCopy({}, true);
    if (m_systemTray.ConsumeRecentRequest() && EnsureRecentCaptures()) {
        if (m_recentCaptures->IsVisible()) m_recentCaptures->Hide();
        else if (!m_recentCaptures->Show(m_systemTray.GetAnchor())) Logger::Error("Cannot show recent captures: {}", SDL_GetError());
    }
    if (!m_recentCaptures) return;
    m_recentCaptures->RenderFrame();
    for (const auto& request : m_recentCaptures->TakeRequests()) {
        using Action = RecentCapturesWindow::Action;
        switch (request.action) {
            case Action::Copy: RequestRecentCopy(request.id); break;
            case Action::History: m_historyPending = true; break;
            case Action::Settings: m_settingsPending = true; break;
            case Action::Edit: OpenHistoryEditor(request.id); break;
            case Action::Save:
            case Action::Pin: RequestRecentImage(request.id, request.action); break;
        }
    }
}
} // namespace shotlet
