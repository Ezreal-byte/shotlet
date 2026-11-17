#include "Application.h"
#include "history/HistoryLibrary.h"
#include "platform/FileService.h"
#include "platform/ApplicationPlatform.h"
#include "platform/TransparentWindow.h"
#include "utils/StringUtils.h"
#include "utils/Logger.h"
#include "ui/OverlayManager.h"
#include "platform/PinWindow.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace shotlet {
namespace {
constexpr float HistoryPinScreenPosition = 80;
constexpr float HistoryPinScaleFactor = 1;
}

void Application::InitializeHistory() {
    if (m_embeddedMode || m_hostCaptureOutput) return;
    try {
        std::unique_ptr<char, decltype(&SDL_free)> path(SDL_GetPrefPath("Shotlet", "Shotlet"), SDL_free);
        if (!path) throw std::runtime_error(SDL_GetError());
        const auto root = std::filesystem::path(std::u8string(path.get(), path.get() + std::char_traits<char>::length(path.get())));
        m_history = std::make_unique<HistoryService>(m_preferences.storageDirectory.empty() ? root : m_preferences.storageDirectory);
    } catch (const std::exception& e) {
        Logger::Error("History initialization failed: {}", e.what());
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, Tr(m_language, TextId::HistoryUnavailable), e.what(), nullptr);
    }
}

bool Application::HandleHistoryLibraryRequest(const HistoryWindow::Request& request, std::string& error) {
    using Action = HistoryWindow::Action;
    auto& store = m_history->Store();
    if (request.action == Action::Reveal) {
        const auto path = store.ResultPath(request.id, error);
        if (!path.empty() && !RevealInFileManager(path)) error = "无法在文件夹中显示截图";
        return true;
    }
    if (request.action == Action::Index) { m_history->RequestIndex(request.id, true); return true; }
    if (request.action == Action::CopyText) {
        auto metadata = store.Get(request.id, error);
        if (!metadata || metadata->details.deletedAtUnixMs) { if (error.empty()) error = "截图已被删除"; return true; }
        if (!metadata->details.ocrIndexed) {
            m_historyCopyTextPending = request.id; m_history->RequestIndex(request.id, true);
            m_historyWindow->SetNotice(Tr(m_language, TextId::HistoryIndexing));
        } else if (!metadata->details.ocrError.empty()) error = metadata->details.ocrError;
        else if (metadata->details.ocrText.empty()) m_historyWindow->SetNotice(Tr(m_language, TextId::HistoryOcrEmpty));
        else if (!SDL_SetClipboardText(metadata->details.ocrText.c_str())) error = SDL_GetError();
        else m_historyWindow->SetNotice(Tr(m_language, TextId::HistoryCopyTextDone));
        return true;
    }
    if (request.action == Action::EmptyTrash) { store.EmptyTrash(error); return true; }
    const bool batch = request.action == Action::Delete || request.action == Action::Restore || request.action == Action::Purge ||
        request.action == Action::Favorite || request.action == Action::Rename || request.action == Action::AddTag || request.action == Action::RemoveTag;
    if (batch) {
        auto ids = request.ids;
        if (ids.empty() && !request.id.empty()) ids.push_back(request.id);
        if (ids.empty() || ids.size() > 100) { error = "请选择截图"; return true; }
        const std::string value = StringUtils::Trim(request.value);
        if ((request.action == Action::Rename || request.action == Action::AddTag) && value.empty()) { error = "名称或标签不能为空"; return true; }
        for (const auto& id : ids) {
            bool ok = false;
            if (request.action == Action::Delete) ok = store.Trash(id, error);
            else if (request.action == Action::Restore) ok = store.Restore(id, error);
            else if (request.action == Action::Purge) ok = store.Delete(id, error);
            else ok = store.UpdateDetails(id, [&](HistoryDetails& details) {
                if (request.action == Action::Favorite) details.favorite = request.enabled;
                else if (request.action == Action::Rename) details.title = value;
                else if (request.action == Action::AddTag && std::find(details.tags.begin(), details.tags.end(), value) == details.tags.end()) details.tags.push_back(value);
                else if (request.action == Action::RemoveTag) std::erase(details.tags, value);
            }, error);
            if (!ok) break;
        }
        return true;
    }
    if (request.action != Action::Export && request.action != Action::Stitch) return false;
    if (m_historyOperation.valid()) { error = "已有导出或拼接任务正在处理"; return true; }
    const auto ids = request.ids.empty() ? std::vector<std::string>{request.id} : request.ids;
    if (ids.empty() || ids.size() > 100 || std::set<std::string>(ids.begin(), ids.end()).size() != ids.size()) {
        error = "选择的截图无效"; return true;
    }
    if (request.action == Action::Export) {
        ActivateAppForSaveDialog(); const auto folder = ChooseExportDirectory(m_language);
        if (m_daemonMode) RestoreActivationPolicy();
        if (folder.empty()) return true;
        auto budget = m_history->ReserveTask(1024u * 1024u);
        if (!budget) { error = "导出所需内存暂不可用，请稍后重试"; return true; }
        auto sharedBudget = std::make_shared<HistoryBudget::Lease>(std::move(budget));
        m_historyOperation = std::async(std::launch::async, [history = m_history.get(), ids, folder, sharedBudget] {
            HistoryOperationResult result; result.budget = sharedBudget;
            history->Store().ExportResults(ids, folder, result.error); return result;
        });
    } else {
        if (ids.size() < 2) { error = "拼接至少需要两张截图"; return true; }
        int64_t width = 0, height = 0; size_t largest = 0;
        for (const auto& id : ids) {
            auto record = store.Get(id, error);
            if (!record || record->details.deletedAtUnixMs) { if (error.empty()) error = "拼接记录已被删除"; return true; }
            width = request.enabled ? width + record->width : std::max<int64_t>(width, record->width);
            height = request.enabled ? std::max<int64_t>(height, record->height) : height + record->height;
            largest = std::max(largest, size_t(record->width) * record->height * 4);
        }
        if (width > 65535 || height > 65535 || !DocumentPixelBytes(int(width), int(height))) { error = "拼接图片尺寸超过上限，请减少图片数量"; return true; }
        // 覆盖合成像素、逐张解码和随后恢复编辑器时的底图副本。
        auto budget = m_history->ReserveTask(size_t(width) * height * 4 * 5 + largest + 16u * 1024u * 1024u);
        if (!budget) { error = "拼接所需内存超出预算，请减少图片数量"; return true; }
        auto sharedBudget = std::make_shared<HistoryBudget::Lease>(std::move(budget));
        m_historyOperation = std::async(std::launch::async, [history = m_history.get(), ids, horizontal = request.enabled, sharedBudget] {
            HistoryOperationResult result; result.budget = sharedBudget;
            result.image = history->Store().Stitch(ids, horizontal, result.error); return result;
        });
    }
    m_historyWindow->SetNotice({}); m_historyWindow->SetBusy(true); return true;
}
void Application::PollHistoryLibrary() {
    if (m_historyOperation.valid() && m_historyOperation.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try {
            auto result = m_historyOperation.get();
            m_historyWindow->SetBusy(false);
            if (!result.error.empty()) m_historyWindow->SetError(result.error);
            else if (result.image) {
                auto document = std::make_shared<CaptureDocument>();
                document->width = result.image.width; document->height = result.image.height;
                const float scale=std::max(1.f,SDL_GetWindowDisplayScale(m_historyWindow->GetSDLWindow()));
                document->logicalSize = {float(document->width)/scale, float(document->height)/scale};
                document->pixelScale = {scale,scale};
#if defined(SHOTLET_PLATFORM_WINDOWS)
                document->documentRenderScale=std::clamp(scale,1.f,3.f);
#endif
                document->createdAtUnixMs = HistoryNow(); document->source = "scroll";
                document->originalRgba.assign(result.image.pixels.get(), result.image.pixels.get() + result.image.ByteSize());
                OpenHistoryDocument(document);
            } else m_historyWindow->SetNotice(Tr(m_language, TextId::HistoryExportDone));
        } catch (const std::exception& e) { m_historyWindow->SetBusy(false); m_historyWindow->SetError(e.what()); }
    }
    if (!m_historyCopyTextPending.empty()) {
        std::string error; auto metadata = m_history->Store().Get(m_historyCopyTextPending, error);
        if (!metadata || metadata->details.deletedAtUnixMs) { m_historyCopyTextPending.clear(); m_historyWindow->SetError("截图已被删除"); }
        else if (metadata->details.ocrIndexed) {
            HistoryWindow::Request request{HistoryWindow::Action::CopyText, m_historyCopyTextPending};
            m_historyCopyTextPending.clear(); HandleHistoryLibraryRequest(request, error);
            if (!error.empty()) m_historyWindow->SetError(error);
        }
    }
}

bool Application::RouteHistoryEvent(const SDL_Event& event) {
    if (m_recentCaptures && m_recentCaptures->ProcessEvent(event)) return true;
    if (m_settingsWindow && m_settingsWindow->ProcessEvent(event)) return true;
    return m_historyWindow && m_historyWindow->ProcessEvent(event);
}

void Application::OpenHistoryEditor(const std::string& id) {
    if (!m_history || m_capturing) return;
    std::string error;
    auto document = m_history->Store().LoadDocument(id, error);
    if (!document) {
        if (m_historyWindow && m_historyWindow->IsVisible()) m_historyWindow->SetError(error, true);
        else if (m_recentCaptures) m_recentCaptures->ShowFeedback(m_systemTray.GetAnchor(), error, true);
        return;
    }
    OpenHistoryDocument(document);
}

void Application::OpenHistoryDocument(std::shared_ptr<const CaptureDocument> document) {
    if(document->sourceRecordId.empty() && document->source=="scroll"){
        m_captureSourceApplication="Shotlet";m_captureSourceWindow=Tr(m_language,TextId::HistoryStitch);
    }
    m_ocrHistoryCandidate.reset();
    if (m_ocrFuture.valid()) { try { (void)m_ocrFuture.get(); } catch (...) {} m_ocrRenderResult = {}; }
    ScreenOverlay::Config config;
    config.language = m_language; config.theme = m_theme;
    if (m_historyWindow) {
        if (auto* window = m_historyWindow->GetSDLWindow()) config.scaleFactor = SDL_GetWindowDisplayScale(window);
        m_restoreHistoryWindow = m_historyWindow->IsVisible();
        m_historyWindow->Hide();
    }
    if (m_recentCaptures) m_recentCaptures->Hide(false);
    if (!m_overlayManager) m_overlayManager = std::make_unique<OverlayManager>();
    m_overlayManager->SetExternalEventCallback([this](const SDL_Event& event) { return RouteHistoryEvent(event); });
    m_overlayManager->SetThemeChangedCallback([this]() { RefreshTheme(true); });
    if (!m_overlayManager->OpenDocument(document, config, m_gpuDevice)) {
        if (m_historyWindow) m_historyWindow->SetError("无法打开编辑文档");
        else if (m_recentCaptures) m_recentCaptures->ShowFeedback(m_systemTray.GetAnchor(), "无法打开编辑文档", true);
        return;
    }
    m_historySourceRecordId = document->sourceRecordId; m_capturing = true;
    m_captureStarted = std::chrono::steady_clock::now(); m_overlayReclaimTimer.Cancel();
}

void Application::ProcessHistory() {
    if (!m_history) return;
    if (m_systemTray.ConsumeHistoryRequest()) m_historyPending = true;
    if (m_capturing) return;
    for (const auto& notice : m_history->TakeNotices()) {
        Logger::Warning("[History] {}", notice);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, Tr(m_language, TextId::HistoryUnavailable), notice.c_str(), nullptr);
    }
    if (!m_running) return;
    if (m_historyPending || m_restoreHistoryWindow) {
        m_historyPending = m_restoreHistoryWindow = false;
        if (!m_historyWindow) m_historyWindow = std::make_unique<HistoryWindow>(m_gpuDevice, *m_history, m_language, m_theme);
        m_historyWindow->Show();
    }
    if (!m_historyWindow) return;
    PollHistoryLibrary();
    if (m_capturing) return;
    m_historyWindow->SetTheme(m_theme); m_historyWindow->RenderFrame();
    for (const auto& request : m_historyWindow->TakeRequests()) {
        std::string error;
        try {
            using Action = HistoryWindow::Action;
            if (HandleHistoryLibraryRequest(request, error)) {
                if (!error.empty()) m_historyWindow->SetError(error);
                if (m_capturing) break;
                continue;
            }
            if (request.action == Action::Edit) { OpenHistoryEditor(request.id); break; }
            HandleHistoryOutputRequest(request, error);
        } catch (const std::exception& e) { error = e.what(); }
        if (!error.empty()) m_historyWindow->SetError(error);
    }
}

void Application::HandleHistoryOutputRequest(const HistoryWindow::Request& request, std::string& error) {
    using Action = HistoryWindow::Action;
    if (request.action == Action::Copy) m_historyWindow->SetError({});
    const auto image = m_history->Store().LoadResult(request.id, false, error);
    if (!image) return;

    if (request.action == Action::Copy) {
        if (!SubmitClipboardCopy(image, std::chrono::steady_clock::now(), ClipboardCopySource::History))
            error = Tr(m_language, TextId::HistoryCopyFailed);
        return;
    }
    if (request.action == Action::Save) {
        ActivateAppForSaveDialog();
        SaveFileWithDialog(image.pixels.get(), image.width, image.height, m_language);
        if (m_daemonMode) RestoreActivationPolicy();
        return;
    }
    if (request.action == Action::Pin) {
        if (PinWindow::Create(image.pixels.get(), image.width, image.height,
                              HistoryPinScreenPosition, HistoryPinScreenPosition,
                              HistoryPinScaleFactor, m_language, m_theme)) {
            m_pinned = true;
        } else {
            error = "创建贴图失败";
        }
    }
}
} // namespace shotlet
