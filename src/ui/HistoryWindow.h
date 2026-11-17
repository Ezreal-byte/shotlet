#pragma once
#include <SDL3/SDL.h>
#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <vector>
#include "Theme.h"
#include "history/HistoryLibrary.h"
#include "history/HistoryService.h"
#include "i18n/Localization.h"
#include "utils/WindowChrome.h"

struct ImRect;

namespace shotlet {
    class HistoryWindow {
    public:
        enum class Action {
            Copy,
            Save,
            Pin,
            Edit,
            Delete,
            CopyText,
            Favorite,
            Rename,
            AddTag,
            RemoveTag,
            Restore,
            Purge,
            EmptyTrash,
            Export,
            Stitch,
            Reveal,
            Index
        };
        struct Request {
            Action action;
            std::string id;
            bool enabled = true;
            std::vector<std::string> ids;
            std::string value;
        };
        HistoryWindow(SDL_GPUDevice *device, HistoryService &service, Language language, Theme theme);
        ~HistoryWindow();
        bool Show();
        void Hide();
        void Close();
        bool IsVisible() const;
        bool ProcessEvent(const SDL_Event &event);
        void RenderFrame();
        void SetTheme(Theme theme);
        void SetLanguage(Language language) {
            m_language = language;
            if (m_window)
                SDL_SetWindowTitle(m_window, Tr(language, TextId::HistoryTitle));
            Invalidate();
        }
        void Invalidate() { m_refreshPending = true; }
        void SetError(std::string error, bool readOnly = false) {
            m_toast.clear();
            m_error = std::move(error);
            if (readOnly)
                m_editable = false;
        }
        std::vector<Request> TakeRequests();
        SDL_Window *GetSDLWindow() const { return m_window; }
        ImGuiContext *GetImGuiContext() const { return m_context; }
        void SetBusy(bool busy) { m_busy = busy; }
        void SetNotice(std::string message) { m_notice = std::move(message); }
        void ShowToast(std::string message) {
            m_toast = std::move(message);
            m_toastUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
        }

    private:
        friend class HistoryWindowTest;
        // Windows 无边框窗口与其他窗口共用 42 逻辑像素标题栏；macOS 沿用容纳原生标题按钮的 52 布局。
#if defined(SHOTLET_PLATFORM_WINDOWS)
        static constexpr float HeaderLogicalHeight = WindowChrome::TitleBarHeight;
#else
        static constexpr float HeaderLogicalHeight = 52;
#endif
        struct SelectionHeaderLayout;
        void ShowPreparedWindow();
        void Refresh();
        void Select(int index);
        void RenderContent();
        void RenderHeader(float width);
        bool IsHeaderDragArea(ImVec2 point, float windowWidth) const;
        void RenderSortCombo();
        void RenderViewModeButtons();
        float RenderSelectionHeader(float width);
        SelectionHeaderLayout CalculateSelectionHeaderLayout(float width) const;
        void RenderSelectAllControl(const SelectionHeaderLayout &layout);
        void RenderSelectionActions(const SelectionHeaderLayout &layout);
        void RenderSelectionFinishControls(const SelectionHeaderLayout &layout);
        void RenderSidebar(float height);
        void RenderNavigationItem(HistoryScope scope, TextId label, const std::string &value = {},
                                  const char *custom = nullptr);
        void RenderStorageQuota(float height);
        void RenderBrowser(float width, float height);
        void RenderEmptyHistory(float width, float height);
        void RenderHistoryCards(float width);
        void RenderDateGroupHeading(int64_t day, size_t count);
        void RenderDetails(float width, float height);
        void RenderDetailsInfo(const HistoryMetadata &record, float width, float height);
        void RenderDetailsPreview(float width);
        void RenderPreviewPopup();
        void RenderDetailsTitle(const HistoryMetadata &record);
        void RenderDetailsField(TextId label, const std::string &value, float width);
        void RenderDetailsMetadata(const HistoryMetadata &record, float width);
        void RenderDetailsTags(const HistoryMetadata &record);
        void RenderOcrText(const HistoryMetadata &record, float width);
        void RenderDetailsActions(const HistoryMetadata &record, float width, float height);
        void RenderCard(int index, ImVec2 size, bool list);
        bool RenderCardFavoriteButton(const HistoryMetadata &record, ImVec2 center);
        void RenderCardPreview(const HistoryMetadata &record, const ImRect &preview, bool visible);
        void RenderCardCaption(const HistoryMetadata &record, const ImRect &bounds, bool list);
        void RenderCardSelection(ImVec2 position, bool selected);
        void RenderCardQuickActions(const HistoryMetadata &record, const ImRect &bounds, const ImRect &preview,
                                    bool list);
        void RenderContextMenu(int index);
        void RenderTagMenu(const std::vector<std::string> &ids);
        void RenderDialogs();
        void RenderStitchDialog();
        void RenderStitchOrder();
        void RenderConfirmDialog();
        void RenderToast();
        void ApplyStyle();
        void HandleWindowAction(WindowChrome::WindowAction action);
        void UpdateFilter(bool resetSelection = false);
        void ClickRecord(int index, bool extend = false, bool toggle = false);
        void SelectAllVisible(bool select);
        void EndMultiSelect();
        void Queue(Action action, const std::string &id = {}, const std::vector<std::string> &ids = {},
                   std::string value = {}, bool enabled = true);
        std::vector<std::string> SelectedIds() const;
        std::set<std::string> AvailableTags() const;
        const HistoryMetadata *FindRecord(const std::string &id) const;
        enum class TagEditor { None, Details, SelectionHeader, ContextMenu };
        void StartTagEdit(std::vector<std::string> ids, TagEditor editor);
        void FinishTagEdit(bool save = true);
        bool RenderTagInput(float width);
        void RenderTag(const HistoryMetadata &record, const std::string &tag, float width);
        void OpenStitch();
        void Confirm(Action action, std::vector<std::string> ids = {});
        SDL_GPUTexture *Thumbnail(const std::string &id);
        void ReleasePreview();
        SDL_GPUDevice *m_device;
        HistoryService &m_service;
        Language m_language;
        Theme m_theme;
        SDL_Window *m_window = nullptr;
        ImGuiContext *m_context = nullptr;
        // 主 ImGui 窗口位于客户区原点，保存的控件矩形可直接用于 SDL 原生命中测试。
        std::vector<ImRect> m_headerControlRects;
        bool m_close = false, m_editable = true;
        std::vector<HistoryMetadata> m_records;
        std::vector<Request> m_requests;
        int m_selected = -1;
        std::string m_selectedId, m_error;
        uint64_t m_totalBytes = 0;
        HistoryImage m_preview;
        SDL_GPUTexture *m_previewTexture = nullptr;
        std::map<std::string, SDL_GPUTexture *> m_thumbnails;
        std::deque<std::string> m_thumbnailOrder;
        std::vector<SDL_GPUTexture *> m_retiredTextures;
        float m_zoom = 1;
        bool m_refreshPending = true;
        int64_t m_nextDay = 0;
        int64_t m_nextExpiry = INT64_MAX;
        HistoryQuery m_query;
        std::vector<size_t> m_visible;
        std::set<std::string> m_selection;
        int m_anchor = -1, m_columns = 1;
        bool m_multiSelect = false, m_listView = false, m_busy = false, m_focusTitle = false,
             m_resetBrowserScroll = true;
        bool m_openStitchDialog = false, m_openConfirmDialog = false;
        TagEditor m_tagEditor = TagEditor::None;
        bool m_focusTagInput = false, m_tagEditorVisible = false;
        bool m_stitchHorizontal = false;
        std::vector<std::string> m_tagTargets, m_stitchOrder, m_confirmTargets;
        Action m_confirmAction = Action::Purge;
        char m_search[512]{}, m_tagInput[129]{}, m_titleInput[513]{};
        std::string m_notice;
        std::string m_toast;
        std::chrono::steady_clock::time_point m_toastUntil{};
        float m_uiScale = 1;
        uint64_t m_revision = 0;
    };
} // namespace shotlet
