#pragma once
#include "PopupSurface.h"
#include "i18n/Localization.h"
#include "recording/ScreenRecorder.h"
#include "utils/PausableClock.h"
#include <chrono>
#include <string>

namespace shotlet {
class RecordingWindow {
public:
    enum class State { Ready, Starting, Recording, Pausing, Paused, Resuming, Saving, Saved, Failed };
    RecordingWindow(SDL_GPUDevice* device, Language language) : m_surface(device), m_language(language) {}
    bool Show(const RecordingOptions& options);
    bool ProcessEvent(const SDL_Event& event);
    void Render(Theme theme);
    void SetState(State state, std::string detail = {});
    void SetDetail(std::string detail);
    const RecordingOptions& Options() const { return m_options; }
    void DisableCamera(std::string error);
    State GetState() const { return m_state; }
    SDL_Window* Window() const { return m_surface.Window(); }
    ImGuiContext* Context() const { return m_surface.Context(); }
    bool ConsumeStopRequest();
    bool ConsumePauseRequest();
    bool HasReachedDurationLimit() const;
    bool ConsumeStartRequest();
    bool ConsumeCloseRequest();
    bool ConsumeRevealRequest();
    bool ConsumePlayRequest();
private:
    friend class RecordingPauseFlowTest;
    void RenderContent();
    void RenderReady();
    void RenderFormatOption(const char* id, const char* label, RecordingFormat format, float x);
    void RenderStatus();
    void RenderRecordingToolbar();
    void RenderPauseButton();
    static bool UsesToolbar(State state);
    void PositionForState();
    void RenderOption(const char* id, const char* label, int icon, bool& enabled, float x, float y, bool available = true);
    PopupSurface m_surface;
    Language m_language;
    State m_state = State::Ready;
    RecordingOptions m_options;
    PausableClock m_clock;
    std::string m_detail;
    bool m_startRequested = false, m_stopRequested = false, m_closeRequested = false, m_revealRequested = false;
    Theme m_theme = Theme::Dark;
    bool m_positionDirty = false;
    bool m_playRequested = false;
    bool m_pauseRequested = false;
};
}
