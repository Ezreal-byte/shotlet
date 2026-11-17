#include "HistoryWindow.h"
#include "WindowChrome.h"
#include "GpuTextureUpload.h"
#include "platform/FontProvider.h"
#include "platform/Appearance.h"
#include "platform/TransparentWindow.h"
#include "utils/ImGuiUtils.h"
#include "utils/UiGpuUtils.h"
#include "utils/UiFontUtils.h"
#include "utils/WindowUtils.h"
#include "utils/StringUtils.h"
#include "utils/ApplicationIcon.h"
#include "utils/Logger.h"
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <cmath>
#if defined(SHOTLET_PLATFORM_MACOS)
#include "platform/ImageEditorWindow.h"
#endif

namespace shotlet {
HistoryWindow::HistoryWindow(SDL_GPUDevice* device, HistoryService& service, Language language, Theme theme)
    : m_device(device), m_service(service), m_language(language), m_theme(theme) {}
HistoryWindow::~HistoryWindow() { Close(); }
bool HistoryWindow::Show() {
    if (m_window) { m_close = false; ShowPreparedWindow(); return true; }
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
#if defined(SHOTLET_PLATFORM_WINDOWS)
    // Windows 使用无边框窗口：header 与共享窗口控制组组成统一标题栏，边缘缩放由命中测试提供。
    flags |= SDL_WINDOW_BORDERLESS;
#endif
    m_window = SDL_CreateWindow(Tr(m_language, TextId::HistoryTitle), 1000, 700, flags);
    if (!m_window) return false;
#if defined(SHOTLET_PLATFORM_WINDOWS)
    if (!ApplicationIcon::SetWindowIcon(m_window))
        Logger::Warning("[HistoryWindow] Cannot set application icon: {}", SDL_GetError());
#endif
#if defined(SHOTLET_PLATFORM_MACOS)
    SetupHistoryBrowserWindow(m_window, m_theme);
#endif
    if (!SDL_ClaimWindowForGPUDevice(m_device, m_window)) { SDL_DestroyWindow(m_window); m_window = nullptr; return false; }
    m_uiScale = WindowUtils::GetDisplayScale(m_window).contentScale;
    int width=int(1280*m_uiScale),height=int(820*m_uiScale);SDL_Rect usable{};
    if(SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(m_window),&usable)){
        width=std::min(width,std::max(800,int(usable.w-32*m_uiScale)));
        height=std::min(height,std::max(500,int(usable.h-32*m_uiScale)));
    }
    SDL_SetWindowSize(m_window,width,height);
    // 无边框窗口不再由系统默认定位：按工作区显式居中，取不到工作区时退回显示器居中。
    if(usable.w>0)
        SDL_SetWindowPosition(m_window,usable.x+(usable.w-width)/2,usable.y+(usable.h-height)/2);
    else
        SDL_SetWindowPosition(m_window,SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED);
    SDL_SetWindowMinimumSize(m_window,std::min(width,int(1000*m_uiScale)),std::min(height,int(640*m_uiScale)));
    SDL_SetWindowHitTest(m_window, [](SDL_Window* window, const SDL_Point* point, void* data) {
        auto* history = static_cast<HistoryWindow*>(data); const float s = history->m_uiScale;
        // 无边框窗口的边缘缩放命中优先于拖动；最大化/全屏时缩放交回系统。
        const auto flags = SDL_GetWindowFlags(window);
        if ((flags & SDL_WINDOW_BORDERLESS) && (flags & SDL_WINDOW_RESIZABLE) &&
            !(flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
            int width = 0, height = 0; SDL_GetWindowSize(window, &width, &height);
            const auto resize = WindowUtils::HitTestResizeBorder(*point, width, height, std::ceil(5.0f * s));
            if (resize != SDL_HITTEST_NORMAL) return resize;
        }
        if (history->m_context) {
            ImGuiUtils::ContextScope context(history->m_context);
            // 保留控件按压和弹出列表的输入；仅有键盘焦点时仍允许拖动空白标题栏。
            const bool pressingControl = ImGui::IsAnyItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
            if (pressingControl ||
                ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
                return SDL_HITTEST_NORMAL;
            }
#if defined(SHOTLET_PLATFORM_WINDOWS)
            // header 承载拖动：位于 ImGui 控件（搜索、排序、视图切换、窗口按钮）之外的区域可拖动。
            int width = 0; SDL_GetWindowSize(window, &width, nullptr);
            if (history->IsHeaderDragArea(ImVec2(float(point->x), float(point->y)), float(width)))
                return SDL_HITTEST_DRAGGABLE;
#else
            int width = 0; SDL_GetWindowSize(window, &width, nullptr);
            return point->y < HeaderLogicalHeight*s && ((point->x > 80*s && point->x < 200*s) ||
                (point->x > 650*s && point->x < width-226*s)) ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
#endif
        }
        return SDL_HITTEST_NORMAL;
    }, this);
    ImGuiContext* previous = ImGui::GetCurrentContext();
    m_context = ImGui::CreateContext(); ImGui::SetCurrentContext(m_context);
    auto& io = ImGui::GetIO(); io.IniFilename = nullptr; io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // 原生控件用 em 字号，ImGui 用字体行高；沿用 FontProvider 的 1.4 倍换算，匹配原生 13 pt。
    // Retina 帧缓冲缩放由 SDL 后端处理，这里不能再次乘屏幕像素密度。
    constexpr float fontSize = 13.0f * 1.4f;
    io.FontDefault = ImGuiUtils::AddUiFont(*io.Fonts, fontSize);
    ImGui_ImplSDL3_InitForOther(m_window);
    ImGui_ImplSDLGPU3_InitInfo info{}; info.Device = m_device;
    info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(m_device, m_window); info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR; info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
    ImGui_ImplSDLGPU3_Init(&info);
    ApplyStyle(); ApplyWindowTheme(m_window, m_theme);
    m_close = false;
    ImGui::SetCurrentContext(previous);
    ShowPreparedWindow();
    return true;
}
void HistoryWindow::ShowPreparedWindow() {
    if (!IsVisible()) {
        Invalidate();
        // 先以透明状态恢复窗口并完成首帧，再显示，避免暴露未绘制的黑色 drawable。
        SDL_SetWindowOpacity(m_window, 0);
        SDL_ShowWindow(m_window);
        RenderFrame();
        SDL_WaitForGPUIdle(m_device);
        SDL_SetWindowOpacity(m_window, 1);
    }
    SDL_RaiseWindow(m_window);
    ForceWindowFocus(m_window);
}
bool HistoryWindow::IsVisible() const { return m_window && !(SDL_GetWindowFlags(m_window) & SDL_WINDOW_HIDDEN); }
void HistoryWindow::Hide() {
    if (!m_window) return;
#if defined(SHOTLET_PLATFORM_MACOS)
    HideHistoryBrowserWindow(m_window);
#else
    SDL_HideWindow(m_window);
    SDL_SyncWindow(m_window);
#endif
}
void HistoryWindow::ReleasePreview() {
    if (m_previewTexture) SDL_ReleaseGPUTexture(m_device, m_previewTexture);
    m_previewTexture = nullptr; m_preview = {};
}
void HistoryWindow::Close() {
    if (!m_window) return;
    SDL_WaitForGPUIdle(m_device); ReleasePreview();
    for (auto& item : m_thumbnails) SDL_ReleaseGPUTexture(m_device, item.second);
    m_thumbnails.clear(); m_thumbnailOrder.clear();
    for (auto* texture : m_retiredTextures) SDL_ReleaseGPUTexture(m_device, texture);
    m_retiredTextures.clear();
    auto* previous = ImGui::GetCurrentContext(); ImGui::SetCurrentContext(m_context);
    ImGui_ImplSDLGPU3_Shutdown(); ImGui_ImplSDL3_Shutdown(); ImGui::DestroyContext(m_context);
    ImGui::SetCurrentContext(previous == m_context ? nullptr : previous); m_context = nullptr;
    SDL_ReleaseWindowFromGPUDevice(m_device, m_window); SDL_DestroyWindow(m_window); m_window = nullptr;
    m_records.clear(); m_visible.clear(); m_selection.clear(); m_anchor = -1;
    m_headerControlRects.clear();
    m_toast.clear();
    m_selected = -1; m_selectedId.clear(); m_requests.clear();
    m_multiSelect=false;m_focusTitle=false;m_resetBrowserScroll=true;
    m_openStitchDialog=m_openConfirmDialog=false;
    m_tagEditor=TagEditor::None;m_tagTargets.clear();m_tagInput[0]=0;
    m_focusTagInput=m_tagEditorVisible=false;
}
void HistoryWindow::SetTheme(Theme theme) {
    if (theme == m_theme) return;
    m_theme = theme; if (!m_context) return;
    auto* previous = ImGui::GetCurrentContext(); ImGui::SetCurrentContext(m_context);
    ApplyStyle(); ApplyWindowTheme(m_window, theme); ImGui::SetCurrentContext(previous);
}
void HistoryWindow::Refresh() {
    // 先取版本，再读取快照；读取期间的新提交由下一帧处理，避免吞掉通知。
    m_revision = m_service.Revision();
    m_refreshPending = false;
    m_nextExpiry = INT64_MAX;
    try {
        auto records = m_service.Store().List(true);
        m_totalBytes = 0; for (const auto& record : records) m_totalBytes += record.bytes;
        auto found = std::find_if(records.begin(), records.end(), [this](const auto& record) { return record.id == m_selectedId; });
        int selected = found == records.end() ? (records.empty() ? -1 : 0) : int(found - records.begin());
        m_records = std::move(records); m_selected = selected;
        for (const auto& record : m_records) {
            const auto deleted = record.details.deletedAtUnixMs;
            if (deleted > 0 && deleted < INT64_MAX - HistoryStore::TrashRetentionMs)
                m_nextExpiry = std::min(m_nextExpiry, deleted + HistoryStore::TrashRetentionMs + 1);
        }
        UpdateFilter();
        for (const auto& record : m_records) if (!record.details.deletedAtUnixMs && !record.details.ocrIndexed)
            m_service.RequestIndex(record.id);
    } catch (const std::exception& e) { m_error = e.what(); }
    m_nextDay = HistoryDayStart(HistoryNow(), 1);
}
void HistoryWindow::Select(int index) {
    m_selected = index;
    const auto id = index < 0 || index >= int(m_records.size()) ? std::string() : m_records[index].id;
    if (id == m_selectedId) return;
    if (m_tagEditor == TagEditor::Details) FinishTagEdit();
    ReleasePreview(); m_selectedId = id; m_zoom = 1; m_error.clear(); m_editable = true;
    if (id.empty()) return;
    const auto& metadata = m_records[index];
    const auto name = HistoryName(metadata,m_language); std::snprintf(m_titleInput, sizeof(m_titleInput), "%s", name.c_str());
    m_editable = metadata.formatVersion == CaptureDocument::FormatVersion && metadata.renderVersion == CaptureDocument::RenderVersion;
    m_preview = m_service.Store().LoadResult(id, false, m_error);
    if (m_preview) m_previewTexture = CreateScreenshotTexture(m_device, m_preview.width, m_preview.height, m_preview.pixels.get(), m_preview.width * 4);
}
SDL_GPUTexture* HistoryWindow::Thumbnail(const std::string& id) {
    auto found = m_thumbnails.find(id); if (found != m_thumbnails.end()) return found->second;
    std::string error; auto image = m_service.Store().LoadResult(id, true, error);
    if (!image) return nullptr;
    auto* texture = CreateScreenshotTexture(m_device, image.width, image.height, image.pixels.get(), image.width * 4);
    if (!texture) return nullptr;
    if (m_thumbnailOrder.size() >= 40) {
        m_retiredTextures.push_back(m_thumbnails[m_thumbnailOrder.front()]);
        m_thumbnails.erase(m_thumbnailOrder.front()); m_thumbnailOrder.pop_front();
    }
    m_thumbnails.emplace(id, texture); m_thumbnailOrder.push_back(id); return texture;
}
bool HistoryWindow::ProcessEvent(const SDL_Event& event) {
    if (!m_window) return false;
    SDL_Window* source = SDL_GetWindowFromEvent(&event);
    if (source != m_window) return false;
    auto* previous = ImGui::GetCurrentContext(); ImGui::SetCurrentContext(m_context); ImGui_ImplSDL3_ProcessEvent(&event);
    if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) m_close = true;
    else if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && m_tagEditor != TagEditor::None) FinishTagEdit(false);
    else if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && !ImGui::GetIO().WantTextInput) {
        const bool popup = ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        const bool command = (event.key.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0;
        if (event.key.key == SDLK_ESCAPE && !popup) {
            if (m_multiSelect) EndMultiSelect();
            else m_close = true;
        } else if (!popup && !m_busy) {
            if (event.key.key == SDLK_A && command) {
                SelectAllVisible(true);
            } else if (event.key.key == SDLK_DOWN || event.key.key == SDLK_UP || event.key.key == SDLK_LEFT || event.key.key == SDLK_RIGHT) {
                auto found = std::find_if(m_visible.begin(), m_visible.end(), [&](size_t i) { return m_records[i].id == m_selectedId; });
                int position = found == m_visible.end() ? 0 : int(found - m_visible.begin());
                const int delta = event.key.key == SDLK_DOWN ? m_columns : event.key.key == SDLK_UP ? -m_columns : event.key.key == SDLK_LEFT ? -1 : 1;
                if (!m_visible.empty()) {
                    position = std::clamp(position + delta, 0, int(m_visible.size()) - 1);
                    const bool shift = (event.key.mod & SDL_KMOD_SHIFT) != 0;
                    if (!shift) { m_selection.clear(); m_multiSelect = false; }
                    ClickRecord(int(m_visible[position]), shift);
                }
            } else if (!m_selectedId.empty()) {
                const bool trash = m_query.scope == HistoryScope::Trash;
                if (event.key.key == SDLK_DELETE || event.key.key == SDLK_BACKSPACE) {
                    auto ids = m_multiSelect ? SelectedIds() : std::vector<std::string>{m_selectedId};
                    if (!ids.empty()) { if (trash) Confirm(Action::Purge, ids); else Queue(Action::Delete, {}, ids); }
                } else if (!trash && (event.key.key == SDLK_RETURN || (event.key.key == SDLK_C && command))) Queue(Action::Copy, m_selectedId);
                else if (!trash && event.key.key == SDLK_F2) m_focusTitle = true;
            }
        }
    }
    ImGui::SetCurrentContext(previous); return true;
}
std::vector<HistoryWindow::Request> HistoryWindow::TakeRequests() { auto requests = std::move(m_requests); m_requests.clear(); return requests; }
bool HistoryWindow::IsHeaderDragArea(ImVec2 point, float windowWidth) const {
    const float height = HeaderLogicalHeight * m_uiScale;
    if (point.x < 0 || point.x >= windowWidth || point.y < 0 || point.y >= height ||
        WindowChrome::IsWindowControlsPoint(point, windowWidth, height, m_uiScale, /*includeMaximize=*/true))
        return false;
    // 原生命中可能早于 ImGui 处理本次鼠标移动，必须使用控件矩形和本次事件坐标。
    return std::none_of(m_headerControlRects.begin(), m_headerControlRects.end(),
                        [point](const ImRect& rect) { return rect.Contains(point); });
}
void HistoryWindow::HandleWindowAction(WindowChrome::WindowAction action) {
    // 无边框窗口控制组动作：与系统按钮行为保持一致。
    if (!m_window) return;
    switch (action) {
        case WindowChrome::WindowAction::Minimize:
            SDL_MinimizeWindow(m_window);
            break;
        case WindowChrome::WindowAction::ToggleMaximize:
            if (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(m_window);
            else SDL_MaximizeWindow(m_window);
            break;
        case WindowChrome::WindowAction::Close:
            m_close = true;
            break;
    }
}
void HistoryWindow::RenderFrame() {
    if (m_close) { Close(); return; }
    // 最小化窗口仍算可见，但 SDL 的 ImGui 后端会把显示尺寸置零，不能继续布局或访问交换链。
    if (!WindowUtils::IsDrawable(m_window)) return;
    if (m_refreshPending || m_revision != m_service.Revision() || HistoryNow() >= m_nextExpiry) Refresh();
    else if (HistoryNow() >= m_nextDay) {
        // 跨天只重新计算内存中的时间筛选，不重读历史文件。
        m_nextDay = HistoryDayStart(HistoryNow(), 1);
        UpdateFilter();
    }
    ImGuiUtils::ContextScope context(m_context);
    const float scale = WindowUtils::GetDisplayScale(m_window).contentScale;
    if (scale != m_uiScale) {
        m_uiScale = scale; ApplyStyle();SDL_SetWindowMinimumSize(m_window,int(1000*scale),int(640*scale));
    }
    ImGui_ImplSDLGPU3_NewFrame(); ImGui_ImplSDL3_NewFrame();
    // 后端更新输入时可能处理原生窗口消息，再检查一次，避免在本帧刚最小化时使用零尺寸布局。
    const auto displaySize = ImGui::GetIO().DisplaySize;
    if (displaySize.x <= 0 || displaySize.y <= 0) return;
    ImGui::NewFrame(); RenderContent(); ImGui::Render();
    ImGuiUtils::SubmitGpuDrawData(m_device, m_window, ImGui::GetDrawData());
    for (auto* texture : m_retiredTextures) SDL_ReleaseGPUTexture(m_device, texture);
    m_retiredTextures.clear();
}

    void HistoryWindow::Queue(Action action, const std::string &id, const std::vector<std::string> &ids,
                              std::string value, bool enabled) {
        Request request{action, id, enabled};
        request.ids = ids;
        request.value = std::move(value);
        m_requests.push_back(std::move(request));
    }

    void HistoryWindow::UpdateFilter(bool resetSelection) {
        // 只规范化筛选查询，保留编辑缓冲区末尾的空格，便于继续输入下一个关键词。
        m_query.search = StringUtils::Trim(m_search);
        m_visible = QueryHistory(m_records, m_query, HistoryNow());
        if (resetSelection) {
            m_selection.clear();
            m_anchor = -1;
            m_resetBrowserScroll = true;
        }
        for (auto it = m_selection.begin(); it != m_selection.end();) {
            const bool visible = std::any_of(m_visible.begin(), m_visible.end(),
                                             [&](size_t index) { return m_records[index].id == *it; });
            if (!visible)
                it = m_selection.erase(it);
            else
                ++it;
        }
        const auto found = std::find_if(m_visible.begin(), m_visible.end(),
                                        [&](size_t index) { return m_records[index].id == m_selectedId; });
        const int index = found == m_visible.end() ? (m_visible.empty() ? -1 : int(m_visible.front())) : int(*found);
        Select(index);
        if (!m_multiSelect && index >= 0) {
            m_selection.clear();
            m_selection.insert(m_selectedId);
        }
    }

    std::vector<std::string> HistoryWindow::SelectedIds() const {
        std::vector<std::string> ids;
        for (size_t index: m_visible)
            if (m_selection.contains(m_records[index].id))
                ids.push_back(m_records[index].id);
        return ids;
    }

    void HistoryWindow::ClickRecord(int index, bool extend, bool toggle) {
        auto found = std::find(m_visible.begin(), m_visible.end(), size_t(index));
        if (found == m_visible.end())
            return;
        const int position = int(found - m_visible.begin());
        if (extend) {
            if (m_anchor < 0 || m_anchor >= int(m_visible.size())) {
                const auto selected = std::find_if(m_visible.begin(), m_visible.end(),
                                                   [&](size_t i) { return m_records[i].id == m_selectedId; });
                m_anchor = selected == m_visible.end() ? position : int(selected - m_visible.begin());
            }
            m_multiSelect = true;
            for (int i = std::min(position, m_anchor); i <= std::max(position, m_anchor); ++i)
                m_selection.insert(m_records[m_visible[i]].id);
        } else if (toggle || m_multiSelect) {
            m_multiSelect = true;
            const auto &id = m_records[index].id;
            if (m_selection.contains(id))
                m_selection.erase(id);
            else
                m_selection.insert(id);
            m_anchor = position;
        } else {
            m_selection = {m_records[index].id};
            m_anchor = position;
        }
        Select(index);
    }

    void HistoryWindow::SelectAllVisible(bool select) {
        if (m_visible.empty() && !m_multiSelect)
            return;
        m_multiSelect = true;
        m_selection.clear();
        m_anchor = -1;
        if (select)
            for (size_t index: m_visible)
                m_selection.insert(m_records[index].id);
    }

    void HistoryWindow::EndMultiSelect() {
        if (m_tagEditor == TagEditor::SelectionHeader)
            FinishTagEdit();
        m_multiSelect = false;
        m_selection.clear();
        m_anchor = -1;
        if (!m_selectedId.empty())
            m_selection.insert(m_selectedId);
    }

    void HistoryWindow::StartTagEdit(std::vector<std::string> ids, TagEditor editor) {
        FinishTagEdit();
        m_tagTargets = std::move(ids);
        m_tagInput[0] = '\0';
        m_tagEditor = editor;
        m_focusTagInput = true;
        m_tagEditorVisible = true;
    }

    void HistoryWindow::FinishTagEdit(bool save) {
        if (m_tagEditor == TagEditor::None)
            return;
        const auto tag = StringUtils::Trim(m_tagInput);
        // 目标记录在开始编辑时固定，切换选中项或输入框退出渲染时仍提交给原目标。
        if (save && !tag.empty())
            Queue(Action::AddTag, {}, m_tagTargets, tag);
        m_tagEditor = TagEditor::None;
        m_tagTargets.clear();
        m_tagInput[0] = '\0';
        m_focusTagInput = false;
    }

    void HistoryWindow::OpenStitch() {
        m_stitchOrder = SelectedIds();
        // 相同时间保持当前排序；缺失记录统一放在末尾，避免比较器解引用空指针。
        std::stable_sort(m_stitchOrder.begin(), m_stitchOrder.end(), [this](const auto &leftId, const auto &rightId) {
            const auto *left = FindRecord(leftId);
            const auto *right = FindRecord(rightId);
            if (!left || !right)
                return left && !right;
            return left->createdAtUnixMs < right->createdAtUnixMs;
        });
        m_openStitchDialog = true;
    }

    void HistoryWindow::Confirm(Action action, std::vector<std::string> ids) {
        m_confirmAction = action;
        m_confirmTargets = std::move(ids);
        m_openConfirmDialog = true;
    }

    const HistoryMetadata *HistoryWindow::FindRecord(const std::string &id) const {
        const auto found =
                std::find_if(m_records.begin(), m_records.end(), [&id](const auto &record) { return record.id == id; });
        return found == m_records.end() ? nullptr : &*found;
    }

    std::set<std::string> HistoryWindow::AvailableTags() const {
        std::set<std::string> tags;
        for (const auto &record: m_records) {
            if (record.details.deletedAtUnixMs)
                continue;
            tags.insert(record.details.tags.begin(), record.details.tags.end());
        }
        return tags;
    }


} // namespace shotlet
