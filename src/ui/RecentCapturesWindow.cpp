#include "RecentCapturesWindow.h"
#include "RecentCapturesLayout.h"
#include "GpuTextureUpload.h"
#include "utils/ImGuiUtils.h"
#include "utils/WindowUtils.h"
#include <algorithm>

namespace shotlet {
using namespace RecentCapturesLayout;

RecentCapturesWindow::RecentCapturesWindow(SDL_GPUDevice* device, HistoryService& service, Language language, Theme theme)
    : m_device(device), m_service(service), m_language(language), m_theme(theme), m_surface(device), m_previewSurface(device) {}
RecentCapturesWindow::~RecentCapturesWindow() { Close(); }

RecentCapturesWindow::Snapshot RecentCapturesWindow::LoadSnapshot(HistoryService& service) {
    Snapshot result;
    auto budget = service.ReserveTask(DisplayBudgetBytes);
    if (!budget) { result.error = "memory"; return result; }
    result.budget = std::make_shared<HistoryBudget::Lease>(std::move(budget));
    const auto records = service.Store().List();
    const auto sorted = QueryHistory(records, {}, HistoryNow());
    for (size_t i = 0; i < std::min(MaxRecords, sorted.size()); ++i) {
        Entry entry; entry.metadata = records[sorted[i]];
        std::string error;
        entry.thumbnail = service.Store().LoadResult(entry.metadata.id, true, error, true);
        result.entries.push_back(std::move(entry));
    }
    return result;
}

bool RecentCapturesWindow::PrepareWindow(SDL_Rect anchor, bool feedbackOnly) {
    if (!m_surface.Create(Tr(m_language, TextId::RecentCapturesTitle), {Width, FeedbackHeight})) return false;
    PositionWindow(anchor, feedbackOnly);
    return true;
}

void RecentCapturesWindow::PositionWindow(SDL_Rect anchor, bool feedbackOnly) {
    m_anchor = anchor;
    const auto display = SDL_GetDisplayForRect(&anchor);
    if (!SDL_GetDisplayUsableBounds(display, &m_workArea)) SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &m_workArea);
    SDL_SetWindowPosition(m_surface.Window(), anchor.x, anchor.y);
    const float scale = m_surface.Scale();
    const int rows = std::max(EmptyBodyRows, int(m_snapshot.entries.size()));
    const int height = feedbackOnly ? FeedbackHeight : HeaderHeight + rows * RowHeight + FooterHeight;
    const SDL_Point size{int(Width * scale), int(height * scale)};
    const auto bounds = WindowUtils::PlaceAnchoredPopup(anchor, size, m_workArea, int(EdgeGap * scale));
    SDL_SetWindowPosition(m_surface.Window(), bounds.x, bounds.y);
    SDL_SetWindowSize(m_surface.Window(), bounds.w, bounds.h);
}

bool RecentCapturesWindow::Show(SDL_Rect anchor) {
    Hide();
    if (!PrepareWindow(anchor, false)) return false;
    // 一次打开使用一份快照；复制不会重排列表，避免鼠标下方的条目跳动。
    if (m_loading.valid() && m_loading.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
        try { (void)m_loading.get(); } catch (...) {}
    }
    ReleaseImages();
    m_snapshot = {};
    m_error.clear(); m_notice.clear(); m_requests.clear();
    m_busy = m_feedbackOnly = false;
    m_previewEnabled = true;
    m_hideAfter = {}; m_selected = -1;
    // 快速关闭再打开时复用尚未完成的读取，不能在主线程等待磁盘任务。
    if (!m_loading.valid()) {
        try { m_loading = std::async(std::launch::async, [&service = m_service] { return LoadSnapshot(service); }); }
        catch (...) { SetError(Tr(m_language, TextId::RecentCapturesUnavailable)); }
    }
    m_focus = RememberPopupFocus();
    SDL_SetWindowFocusable(m_surface.Window(), true);
    m_surface.Show(true);
    RenderFrame();
    return true;
}

void RecentCapturesWindow::Hide(bool restoreFocus) {
    m_previewSurface.Hide();
    // 先交还焦点，再隐藏窗口，Windows 才能判断前台窗口是否仍是当前弹层。
    ReleasePopupFocus(m_focus, m_surface.Window(), restoreFocus);
    m_focus = nullptr;
    m_surface.Hide();
    m_hideAfter = {};
}

void RecentCapturesWindow::ReleaseImages() {
    for (auto& entry : m_snapshot.entries) {
        if (entry.texture) SDL_ReleaseGPUTexture(m_device, entry.texture);
        entry.texture = nullptr;
    }
    if (m_previewTexture) SDL_ReleaseGPUTexture(m_device, m_previewTexture);
    m_previewTexture = nullptr; m_preview = {}; m_previewId.clear(); m_requestedPreviewId.clear();
}

void RecentCapturesWindow::Close() {
    Hide(false);
    // 所有后台任务借用 service，迁移历史目录和退出前必须先等待任务释放借用。
    if (m_loading.valid()) m_loading.wait();
    if (m_loadingPreview.valid()) m_loadingPreview.wait();
    m_loading = {}; m_loadingPreview = {};
    if (m_surface.Window()) SDL_WaitForGPUIdle(m_device);
    ReleaseImages(); m_snapshot = {};
    m_previewSurface.Close(); m_surface.Close();
}

void RecentCapturesWindow::PollLoads() {
    constexpr auto ready = std::chrono::seconds(0);
    if (m_loading.valid() && m_loading.wait_for(ready) == std::future_status::ready) {
        try {
            auto snapshot = m_loading.get();
            if (!m_feedbackOnly) {
                ReleaseImages(); m_snapshot = std::move(snapshot);
                if (!m_snapshot.error.empty()) SetError(Tr(m_language, TextId::RecentCapturesUnavailable));
                for (auto& entry : m_snapshot.entries)
                    if (entry.thumbnail) entry.texture = CreateScreenshotTexture(m_device, entry.thumbnail.width,
                        entry.thumbnail.height, entry.thumbnail.pixels.get(), entry.thumbnail.width * 4);
                if (!m_snapshot.entries.empty()) Select(0);
                PositionWindow(m_anchor, false);
            }
        } catch (...) { SetError(Tr(m_language, TextId::RecentCapturesUnavailable)); }
    }
    if (m_loadingPreview.valid() && m_loadingPreview.wait_for(ready) == std::future_status::ready) {
        try {
            auto preview = m_loadingPreview.get();
            if (preview.id == m_requestedPreviewId && preview.image) {
                if (m_previewTexture) SDL_ReleaseGPUTexture(m_device, m_previewTexture);
                m_preview = std::move(preview.image); m_previewId = std::move(preview.id);
                m_previewTexture = CreateScreenshotTexture(m_device, m_preview.width, m_preview.height,
                    m_preview.pixels.get(), m_preview.width * 4);
            }
        } catch (...) { /* 清晰预览失败时保留已有缩略图，复制仍会重新读取最终图片。 */ }
    }
}

void RecentCapturesWindow::StartPreview() {
    if (m_selected < 0 || m_loadingPreview.valid()) return;
    const auto metadata = m_snapshot.entries[size_t(m_selected)].metadata;
    if (metadata.id == m_requestedPreviewId) return;
    m_requestedPreviewId = metadata.id;
    m_loadingPreview = std::async(std::launch::async, [&service = m_service, metadata] {
        Preview result; result.id = metadata.id;
        const auto bytes = DocumentPixelBytes(metadata.width, metadata.height);
        if (!bytes) return result;
        auto budget = service.ReserveTask(*bytes * DecodeBufferCopies);
        if (!budget) return result;
        std::string error;
        const auto image = service.Store().LoadResult(metadata.id, false, error, true);
        if (image) result.image = MakeHistoryThumbnail(image, PreviewPixelWidth, PreviewPixelHeight);
        return result;
    });
}

void RecentCapturesWindow::Select(int index) {
    if (index == m_selected) return;
    m_selected = index;
    m_previewDue = Clock::now() + PreviewDelay;
    m_requestedPreviewId.clear();
}

void RecentCapturesWindow::Queue(Action action, const std::string& id) {
    if (m_busy) return;
    m_requests.push_back({action, id});
    m_previewSurface.Hide();
    if (action == Action::Copy) {
        m_busy = true; m_notice = Tr(m_language, TextId::RecentCapturesCopying); m_error.clear(); m_hideAfter = {};
    }
    else Hide(false);
}

bool RecentCapturesWindow::ProcessEvent(const SDL_Event& event) {
    if (m_previewSurface.ProcessEvent(event)) return true;
    if (!m_surface.ProcessEvent(event)) return false;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST && !m_feedbackOnly &&
        !(SDL_GetWindowFlags(m_surface.Window()) & SDL_WINDOW_INPUT_FOCUS)) Hide(false);
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) Hide();
    if (event.type == SDL_EVENT_MOUSE_MOTION) m_mouseMoved = true;
    if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat) return true;
    if (event.key.key == SDLK_ESCAPE) { Hide(); return true; }
    if (m_busy || m_snapshot.entries.empty()) return true;
    const int last = int(m_snapshot.entries.size()) - 1;
    if (event.key.key == SDLK_UP || event.key.key == SDLK_DOWN) {
        Select(std::clamp(m_selected + (event.key.key == SDLK_UP ? -1 : 1), 0, last));
        m_previewEnabled = true;
        m_scrollToSelection = true;
    } else if (event.key.key == SDLK_SPACE) {
        m_previewEnabled = !m_previewSurface.IsVisible(); m_previewDue = Clock::now();
        if (!m_previewEnabled) m_previewSurface.Hide();
    } else if (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER ||
               (event.key.key == SDLK_C && (event.key.mod & (SDL_KMOD_GUI | SDL_KMOD_CTRL)))) {
        Queue(Action::Copy, m_snapshot.entries[size_t(std::max(0, m_selected))].metadata.id);
    }
    return true;
}

void RecentCapturesWindow::SetCopyResult(bool copied) {
    m_busy = false;
    if (!copied) { SetError(Tr(m_language, TextId::HistoryCopyFailed)); return; }
    m_error.clear(); m_notice = Tr(m_language, TextId::RecentCapturesCopied);
    m_hideAfter = Clock::now() + CopyFeedbackDuration;
}
void RecentCapturesWindow::SetError(std::string error) { m_busy = false; m_error = std::move(error); m_notice.clear(); }

void RecentCapturesWindow::ShowFeedback(SDL_Rect anchor, std::string message, bool error) {
    Hide(false);
    if (!PrepareWindow(anchor, true)) return;
    m_feedbackOnly = true; m_busy = false;
    m_error = error ? std::move(message) : std::string();
    m_notice = error ? std::string() : std::move(message);
    m_hideAfter = Clock::now() + FeedbackDuration;
    SDL_SetWindowFocusable(m_surface.Window(), false);
    m_surface.Show(false);
    RenderFrame();
}

std::vector<RecentCapturesWindow::Request> RecentCapturesWindow::TakeRequests() {
    auto requests = std::move(m_requests); m_requests.clear(); return requests;
}

void RecentCapturesWindow::RenderFrame() {
    PollLoads();
    if (!IsVisible()) return;
    if (m_hideAfter != Clock::time_point{} && Clock::now() >= m_hideAfter) { Hide(); return; }
    m_surface.Render(m_theme, [this] { RenderContent(); });
    if (!m_busy && !m_feedbackOnly && m_hideAfter == Clock::time_point{}) RenderPreview();
    m_mouseMoved = false;
}
} // namespace shotlet
