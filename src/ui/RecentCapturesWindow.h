#pragma once
#include "PopupSurface.h"
#include "history/HistoryLibrary.h"
#include "history/HistoryService.h"
#include "platform/NativeSystemTray.h"
#include <future>
#include <chrono>

namespace shotlet {
class RecentCapturesWindow {
public:
    static constexpr size_t MaxRecords = 8;
    enum class Action { Copy, History, Settings, Edit, Save, Pin };
    struct Request { Action action; std::string id; };
    RecentCapturesWindow(SDL_GPUDevice* device, HistoryService& service, Language language, Theme theme);
    ~RecentCapturesWindow();
    bool Show(SDL_Rect anchor);
    void Hide(bool restoreFocus = true);
    void Close();
    bool IsVisible() const { return m_surface.IsVisible(); }
    bool ProcessEvent(const SDL_Event& event);
    void RenderFrame();
    void SetTheme(Theme theme) { m_theme = theme; }
    void SetLanguage(Language language) { m_language = language; }
    void SetCopyResult(bool copied);
    void SetError(std::string error);
    void ShowFeedback(SDL_Rect anchor, std::string message, bool error = false);
    std::vector<Request> TakeRequests();
    SDL_Window* GetSDLWindow() const { return m_surface.Window(); }
    ImGuiContext* GetImGuiContext() const { return m_surface.Context(); }
private:
    friend class RecentCapturesWindowTest;
    using Clock = std::chrono::steady_clock;
    struct Entry { HistoryMetadata metadata; HistoryImage thumbnail; SDL_GPUTexture* texture = nullptr; };
    struct Snapshot {
        std::shared_ptr<HistoryBudget::Lease> budget;
        std::vector<Entry> entries;
        std::string error;
    };
    struct Preview { std::string id; HistoryImage image; };
    static Snapshot LoadSnapshot(HistoryService& service);
    bool PrepareWindow(SDL_Rect anchor, bool feedbackOnly);
    void PositionWindow(SDL_Rect anchor, bool feedbackOnly);
    void PollLoads();
    void StartPreview();
    void Select(int index);
    void Queue(Action action, const std::string& id = {});
    void ReleaseImages();
    void RenderContent();
    void RenderEntry(size_t index);
    void RenderFooter();
    void RenderPreview();
    void RenderPreviewContent();
    void PositionPreview();
    SDL_GPUDevice* m_device;
    HistoryService& m_service;
    Language m_language;
    Theme m_theme;
    PopupSurface m_surface, m_previewSurface;
    PopupFocusToken* m_focus = nullptr;
    Snapshot m_snapshot;
    std::future<Snapshot> m_loading;
    std::future<Preview> m_loadingPreview;
    HistoryImage m_preview;
    SDL_GPUTexture* m_previewTexture = nullptr;
    std::string m_previewId, m_requestedPreviewId;
    std::vector<Request> m_requests;
    int m_selected = -1;
    bool m_busy = false, m_feedbackOnly = false, m_mouseMoved = false, m_previewEnabled = true, m_scrollToSelection = false;
    std::string m_error, m_notice;
    SDL_Rect m_workArea{};
    SDL_Rect m_anchor{};
    Clock::time_point m_previewDue{}, m_hideAfter{};
};
} // namespace shotlet
