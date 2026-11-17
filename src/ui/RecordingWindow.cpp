#include "RecordingWindow.h"
#include "ToolbarIcons.h"
#include "utils/Colors.h"
#include "utils/FormatUtils.h"
#include "utils/WindowUtils.h"

namespace shotlet {
namespace Layout {
constexpr int ReadyWidth = 360, ReadyHeight = 192, ErrorHeight = 36;
constexpr int StatusWidth = 380, StatusHeight = 160;
constexpr float Padding = 18, ButtonTop = 15, StartHeight = 44, HeaderHeight = 72;
constexpr float FormatTop = 88, FormatLabelLeft = 26, Mp4Left = 120, GifLeft = 215;
constexpr float OptionsTop = 125, OptionHeight = 56, IconSize = 28, LabelGap = 5;
constexpr float OptionWidth = (ReadyWidth - Padding * 2) / 4;
constexpr float ButtonRadius = 8, IconStroke = 1.7f, DisabledSlashStroke = 2.2f;
constexpr float ErrorTop = 185, DragHeight = 34, StatusButtonHeight = 30;
constexpr float RadioRadius = 9, RadioDotRadius = 3, RadioTextGap = 10, RadioStroke = 1.4f;
constexpr int EdgeGap = 12;
constexpr int ControlWidth = 416, ControlHeight = 52, EnglishControlExtraWidth = 36;
constexpr float ControlInset = 16, GripWidth = 12, GripDotRadius = 1.3f, GripDotGap = 5;
constexpr int GripColumns = 2, GripRows = 3;
constexpr float PauseLeft = 44, PauseButtonWidth = 36, PauseIconWidth = 18, PauseIconHeight = 20;
constexpr float PauseBarWidth = 6, PauseBarRadius = 1.5f, ControlTimeLeft = 94;
constexpr float StopWidth = 90, StopHeight = 36, ControlHoverRadius = 6;
constexpr float ControlFontSize = 18;
}

bool RecordingWindow::UsesToolbar(State state) {
    return state != State::Ready && state != State::Saved && state != State::Failed;
}

bool RecordingWindow::Show(const RecordingOptions& options) {
    m_options = options;
    if (!m_surface.Create(Tr(m_language, TextId::RecordingTitle), {Layout::ReadyWidth, Layout::ReadyHeight})) return false;
    PositionForState();
    SDL_SetWindowHitTest(m_surface.Window(), [](SDL_Window* window, const SDL_Point* point, void* data) {
        const auto& control = *static_cast<RecordingWindow*>(data);
        if (control.m_state == State::Ready) return SDL_HITTEST_NORMAL;
        if (UsesToolbar(control.m_state)) {
            const float dragWidth = (Layout::ControlInset + Layout::GripWidth) * WindowUtils::GetDisplayScale(window).contentScale;
            return point->x < dragWidth ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
        }
        const float height = Layout::DragHeight * WindowUtils::GetDisplayScale(window).contentScale;
        return point->y < height ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
    }, this);
    ExcludeRecordingControlWindow(m_surface.Window());
    m_surface.Show(true);
    return true;
}

void RecordingWindow::PositionForState() {
    if (!m_surface.Window()) return;
    const auto& region = m_options.region;
    SDL_Rect anchor{int(region.x), int(region.y), int(region.width), int(region.height)};
    SDL_SetWindowPosition(m_surface.Window(), anchor.x, anchor.y);
    const float scale = m_surface.Scale();
    const bool ready = m_state == State::Ready;
    const bool toolbar = UsesToolbar(m_state);
    const int controlWidth = Layout::ControlWidth + (m_language == Language::English ? Layout::EnglishControlExtraWidth : 0);
    const SDL_Point size{int((ready ? Layout::ReadyWidth : toolbar ? controlWidth : Layout::StatusWidth) * scale),
        int((ready ? Layout::ReadyHeight + (m_detail.empty() ? 0 : Layout::ErrorHeight) : toolbar ? Layout::ControlHeight : Layout::StatusHeight) * scale)};
    SDL_Rect work{};
    if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForRect(&anchor), &work)) SDL_GetDisplayBounds(SDL_GetPrimaryDisplay(), &work);
    const auto placement = ready ? WindowUtils::PlaceCenteredPopup(anchor, size, work)
        : WindowUtils::PlaceAnchoredPopup(anchor, size, work, int(Layout::EdgeGap * scale));
    SDL_SetWindowSize(m_surface.Window(), placement.w, placement.h);
    SDL_SetWindowPosition(m_surface.Window(), placement.x, placement.y);
}

void RecordingWindow::SetState(State state, std::string detail) {
    const State previous = m_state;
    m_state = state; m_detail = std::move(detail);
    if (state == State::Recording) {
        if (previous == State::Starting || previous == State::Ready) m_clock.Reset();
        else m_clock.SetPaused(false);
    } else if (UsesToolbar(state) && state != State::Starting) m_clock.SetPaused(true);
    // 暂停和继续不改变窗口尺寸或位置，保留用户通过手柄拖动的位置。
    if (previous != state && (UsesToolbar(previous) != UsesToolbar(state) || previous == State::Ready)) PositionForState();
}
void RecordingWindow::SetDetail(std::string detail) { m_detail = std::move(detail); if (m_state == State::Ready) PositionForState(); }
void RecordingWindow::DisableCamera(std::string error) { m_options.camera = false; SetDetail(std::move(error)); }

bool RecordingWindow::ProcessEvent(const SDL_Event& event) {
    if (!m_surface.ProcessEvent(event)) return false;
    if (m_state == State::Ready && event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_RETURN && !event.key.repeat)
        m_startRequested = true;
    if ((m_state == State::Recording || m_state == State::Paused) && event.type == SDL_EVENT_KEY_DOWN &&
        event.key.key == SDLK_SPACE && !event.key.repeat) m_pauseRequested = true;
    const bool close = event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED ||
        (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE && !event.key.repeat);
    if (close) {
        if (UsesToolbar(m_state) && m_state != State::Saving) m_stopRequested = true;
        else if (m_state != State::Saving) m_closeRequested = true;
    }
    return true;
}

void RecordingWindow::Render(Theme theme) {
    m_theme = UsesToolbar(m_state) ? Theme::Dark : theme;
    m_surface.Render(m_theme, [this] { RenderContent(); });
    // 等本帧提交后再调整窗口，避免在 ImGui 正在绘制时改变交换链尺寸。
    if (m_positionDirty) { m_positionDirty = false; PositionForState(); }
}
void RecordingWindow::RenderContent() {
    if (m_state == State::Ready) RenderReady();
    else if (UsesToolbar(m_state)) RenderRecordingToolbar();
    else RenderStatus();
}

bool RecordingWindow::HasReachedDurationLimit() const {
    return UsesToolbar(m_state) && m_state != State::Starting && m_state != State::Saving && m_clock.Elapsed() >= MaximumRecordingDuration;
}

void RecordingWindow::RenderPauseButton() {
    const float scale = m_surface.Scale();
    const bool paused = m_state == State::Paused || m_state == State::Resuming;
    const bool available = m_state == State::Recording || m_state == State::Paused;
    const float top = (Layout::ControlHeight - Layout::PauseButtonWidth) / 2;
    ImGui::SetCursorPos(ImVec2(Layout::PauseLeft * scale, top * scale));
    ImGui::BeginDisabled(!available);
    if (ImGui::InvisibleButton("record-pause", ImVec2(Layout::PauseButtonWidth * scale, Layout::PauseButtonWidth * scale))) m_pauseRequested = true;
    ImGui::EndDisabled();
    auto* draw = ImGui::GetWindowDrawList();
    const auto start = ImGui::GetItemRectMin(), end = ImGui::GetItemRectMax();
    if (available && ImGui::IsItemHovered()) {
        draw->AddRectFilled(start, end, Colors::RecordingDarkOptions, Layout::ControlHoverRadius * scale);
        ImGui::SetTooltip("%s", Tr(m_language, paused ? TextId::RecordingResumeCapture : TextId::RecordingPauseCapture));
    }
    const ImU32 color = available ? Colors::White : Colors::RecordingControlGrip;
    const ImVec2 icon(start.x + (Layout::PauseButtonWidth - Layout::PauseIconWidth) * scale / 2,
        start.y + (Layout::PauseButtonWidth - Layout::PauseIconHeight) * scale / 2);
    if (paused) {
        draw->AddTriangleFilled(icon, ImVec2(icon.x, icon.y + Layout::PauseIconHeight * scale),
            ImVec2(icon.x + Layout::PauseIconWidth * scale, icon.y + Layout::PauseIconHeight * scale / 2), color);
    } else {
        for (float offset : {0.0f, Layout::PauseIconWidth - Layout::PauseBarWidth})
            draw->AddRectFilled(ImVec2(icon.x + offset * scale, icon.y),
                ImVec2(icon.x + (offset + Layout::PauseBarWidth) * scale, icon.y + Layout::PauseIconHeight * scale),
                color, Layout::PauseBarRadius * scale);
    }
}

void RecordingWindow::RenderRecordingToolbar() {
    ImGui::PushFont(nullptr, Layout::ControlFontSize);
    const float scale = m_surface.Scale();
    auto* draw = ImGui::GetWindowDrawList();
    const auto origin = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), Colors::RecordingDarkBackground,
        PopupSurface::WindowCornerRadiusLogical * scale);
    const float gripTop = (Layout::ControlHeight - (Layout::GripRows - 1) * Layout::GripDotGap) / 2;
    for (int row = 0; row < Layout::GripRows; ++row)
        for (int column = 0; column < Layout::GripColumns; ++column)
            draw->AddCircleFilled(ImVec2(origin.x + (Layout::ControlInset + column * Layout::GripDotGap) * scale,
                origin.y + (gripTop + row * Layout::GripDotGap) * scale), Layout::GripDotRadius * scale, Colors::RecordingControlGrip);
    RenderPauseButton();
    const auto elapsed = m_state == State::Starting ? std::chrono::seconds::zero() :
        std::chrono::duration_cast<std::chrono::seconds>(std::min(m_clock.Elapsed(), PausableClock::Clock::duration(MaximumRecordingDuration)));
    std::string text = FormatUtils::HoursMinutesSeconds(elapsed) + "  /  " + FormatUtils::HoursMinutesSeconds(MaximumRecordingDuration);
    if (m_state == State::Starting) text = Tr(m_language, TextId::RecordingStarting);
    else if (m_state == State::Saving) text = Tr(m_language, TextId::RecordingSaving);
    const auto textSize = ImGui::CalcTextSize(text.c_str());
    draw->AddText(ImVec2(origin.x + Layout::ControlTimeLeft * scale, origin.y + (size.y - textSize.y) / 2), Colors::RecordingControlText, text.c_str());
    const float stopWidth = (Layout::StopWidth + (m_language == Language::English ? Layout::EnglishControlExtraWidth : 0)) * scale;
    ImGui::SetCursorPos(ImVec2(size.x - stopWidth - Layout::ControlInset * scale, (size.y - Layout::StopHeight * scale) / 2));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Layout::ButtonRadius * scale);
    ImGui::PushStyleColor(ImGuiCol_Button, Colors::RecordingStop);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Colors::RecordingStopHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Colors::RecordingStopPressed);
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::White);
    ImGui::BeginDisabled(m_state == State::Saving);
    if (ImGui::Button(Tr(m_language, TextId::RecordingEnd), ImVec2(stopWidth, Layout::StopHeight * scale))) m_stopRequested = true;
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4); ImGui::PopStyleVar();
    if (!m_detail.empty()) {
        draw->AddRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), Colors::RecordingStop,
            PopupSurface::WindowCornerRadiusLogical * scale);
        if (ImGui::IsWindowHovered()) ImGui::SetTooltip("%s", m_detail.c_str());
    }
    ImGui::PopFont();
}

void RecordingWindow::RenderOption(const char* id, const char* label, int icon, bool& enabled, float x, float y, bool available) {
    const float scale = m_surface.Scale();
    const auto& palette = GetThemePalette(m_theme);
    ImGui::SetCursorPos(ImVec2(x * scale, y * scale));
    ImGui::BeginDisabled(!available);
    if (ImGui::InvisibleButton(id, ImVec2(Layout::OptionWidth * scale, Layout::OptionHeight * scale))) {
        enabled = !enabled;
        if (!m_detail.empty()) { m_detail.clear(); m_positionDirty = true; }
    }
    ImGui::EndDisabled();
    if (!available && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", Tr(m_language, TextId::RecordingGifSilent));
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 origin = ImGui::GetWindowPos();
    const float iconSize = Layout::IconSize * scale;
    const ImVec2 position(origin.x + (x + (Layout::OptionWidth - Layout::IconSize) / 2) * scale, origin.y + y * scale);
    const ImU32 color = available && enabled ? palette.icon : palette.disabledIcon;
    ToolbarIcons::DrawIcon(draw, position, iconSize, static_cast<ToolbarIcons::IconType>(icon), color, Layout::IconStroke * scale);
    if (available && !enabled) draw->AddLine(position, ImVec2(position.x + iconSize, position.y + iconSize), palette.danger, Layout::DisabledSlashStroke * scale);
    const auto textSize = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(origin.x + (x + Layout::OptionWidth / 2) * scale - textSize.x / 2,
        position.y + iconSize + Layout::LabelGap * scale), color, label);
}

void RecordingWindow::RenderFormatOption(const char* id, const char* label, RecordingFormat format, float x) {
    const float scale = m_surface.Scale();
    const auto& palette = GetThemePalette(m_theme);
    const auto textSize = ImGui::CalcTextSize(label);
    const float height = ImGui::GetFrameHeight();
    ImGui::SetCursorPos(ImVec2(x * scale, Layout::FormatTop * scale));
    const auto position = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton(id, ImVec2((Layout::RadioRadius * 2 + Layout::RadioTextGap) * scale + textSize.x, height)))
        m_options.format = format;
    const bool active = m_options.format == format;
    auto* draw = ImGui::GetWindowDrawList();
    const ImVec2 center(position.x + Layout::RadioRadius * scale, position.y + height / 2);
    if (active) {
        draw->AddCircleFilled(center, Layout::RadioRadius * scale, Colors::RecordingAccent);
        draw->AddCircleFilled(center, Layout::RadioDotRadius * scale, Colors::White);
    } else draw->AddCircle(center, Layout::RadioRadius * scale, palette.disabledIcon, 0, Layout::RadioStroke * scale);
    draw->AddText(ImVec2(position.x + (Layout::RadioRadius * 2 + Layout::RadioTextGap) * scale,
        position.y + (height - textSize.y) / 2), active ? palette.text : palette.secondaryText, label);
}

void RecordingWindow::RenderReady() {
    const float scale = m_surface.Scale();
    const auto& palette = GetThemePalette(m_theme);
    auto* draw = ImGui::GetWindowDrawList();
    const auto origin = ImGui::GetWindowPos();
    const auto size = ImGui::GetWindowSize();
    const ImU32 background = m_theme == Theme::Dark ? Colors::RecordingDarkBackground : palette.panelBackground;
    const ImU32 options = m_theme == Theme::Dark ? Colors::RecordingDarkOptions : palette.editorToolbarBackground;
    draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y), background);
    draw->AddRectFilled(ImVec2(origin.x, origin.y + Layout::HeaderHeight * scale), ImVec2(origin.x + size.x, origin.y + size.y), options);
    ImGui::SetCursorPos(ImVec2(Layout::Padding * scale, Layout::ButtonTop * scale));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, Layout::ButtonRadius * scale);
    ImGui::PushStyleColor(ImGuiCol_Button, Colors::RecordingAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Colors::RecordingAccentHovered);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, Colors::RecordingAccentPressed);
    ImGui::PushStyleColor(ImGuiCol_Text, Colors::White);
    if (ImGui::Button(Tr(m_language, TextId::RecordingStart), ImVec2((Layout::ReadyWidth - Layout::Padding * 2) * scale, Layout::StartHeight * scale)))
        m_startRequested = true;
    ImGui::PopStyleColor(4); ImGui::PopStyleVar();
    ImGui::SetCursorPos(ImVec2(Layout::FormatLabelLeft * scale, Layout::FormatTop * scale));
    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("%s", Tr(m_language, TextId::RecordingFormat));
    RenderFormatOption("record-format-mp4", "MP4", RecordingFormat::Mp4, Layout::Mp4Left);
    RenderFormatOption("record-format-gif", "GIF", RecordingFormat::Gif, Layout::GifLeft);
    const bool audio = m_options.format == RecordingFormat::Mp4;
    RenderOption("record-system-audio", Tr(m_language, TextId::RecordingSpeaker), int(ToolbarIcons::IconType::Speaker), m_options.systemAudio,
        Layout::Padding, Layout::OptionsTop, audio);
    RenderOption("record-microphone", Tr(m_language, TextId::RecordingMicrophone), int(ToolbarIcons::IconType::Microphone), m_options.microphone,
        Layout::Padding + Layout::OptionWidth, Layout::OptionsTop, audio);
    RenderOption("record-camera", Tr(m_language, TextId::RecordingCamera), int(ToolbarIcons::IconType::Camera), m_options.camera,
        Layout::Padding + Layout::OptionWidth * 2, Layout::OptionsTop);
    RenderOption("record-cursor", Tr(m_language, TextId::RecordingCursor), int(ToolbarIcons::IconType::Cursor), m_options.showCursor,
        Layout::Padding + Layout::OptionWidth * 3, Layout::OptionsTop);
    if (!m_detail.empty()) {
        ImGui::SetCursorPos(ImVec2(Layout::Padding * scale, Layout::ErrorTop * scale));
        ImGui::PushStyleColor(ImGuiCol_Text, palette.danger);
        ImGui::TextUnformatted(m_detail.c_str());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m_detail.c_str());
        ImGui::PopStyleColor();
    }
}

void RecordingWindow::RenderStatus() {
    const auto title = m_state == State::Saved ? TextId::RecordingSaved : TextId::RecordingFailed;
    ImGui::TextUnformatted(Tr(m_language, title));
    const auto fps = m_options.format == RecordingFormat::Gif ? GifFramesPerSecond : RecordingFramesPerSecond;
    ImGui::TextDisabled("%s · %d × %d · %d FPS", m_options.format == RecordingFormat::Gif ? "GIF" : "MP4", m_options.widthPixels, m_options.heightPixels, fps);
    if (!m_detail.empty()) { ImGui::TextUnformatted(m_detail.c_str()); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m_detail.c_str()); }
    const ImVec2 buttonSize(0, Layout::StatusButtonHeight * m_surface.Scale());
    if (m_state == State::Saved) {
        if (ImGui::Button(Tr(m_language, TextId::RecordingPlay), buttonSize)) m_playRequested = true;
        ImGui::SameLine();
        if (ImGui::Button(Tr(m_language, TextId::RecordingReveal), buttonSize)) m_revealRequested = true;
        ImGui::SameLine();
    }
    if (ImGui::Button(Tr(m_language, TextId::RecordingClose), buttonSize)) m_closeRequested = true;
}

bool RecordingWindow::ConsumeStartRequest() { const bool value = m_startRequested; m_startRequested = false; return value; }
bool RecordingWindow::ConsumeStopRequest() { const bool value = m_stopRequested; m_stopRequested = false; return value; }
bool RecordingWindow::ConsumePauseRequest() { const bool value = m_pauseRequested; m_pauseRequested = false; return value; }
bool RecordingWindow::ConsumeCloseRequest() { const bool value = m_closeRequested; m_closeRequested = false; return value; }
bool RecordingWindow::ConsumeRevealRequest() { const bool value = m_revealRequested; m_revealRequested = false; return value; }
bool RecordingWindow::ConsumePlayRequest() { const bool value = m_playRequested; m_playRequested = false; return value; }
}
