#include "RecentCapturesWindow.h"
#include "RecentCapturesLayout.h"
#include "utils/ImGuiUtils.h"
#include "utils/WindowUtils.h"
#include <algorithm>
#include <format>

namespace shotlet {
using namespace RecentCapturesLayout;

void RecentCapturesWindow::RenderContent() {
    ImGui::TextUnformatted(Tr(m_language, TextId::RecentCapturesTitle));
    if (m_feedbackOnly) {
        ImGui::Spacing();
        ImGui::TextWrapped("%s", m_error.empty() ? m_notice.c_str() : m_error.c_str());
        return;
    }
    const auto* hint = !m_error.empty() ? m_error.c_str() : !m_notice.empty() ? m_notice.c_str()
                                                                        : Tr(m_language, TextId::RecentCapturesHint);
    ImGui::TextDisabled("%s", hint);
    ImGui::Separator();
    const float bodyHeight = std::max(1.0f, ImGui::GetContentRegionAvail().y - FooterHeight * m_surface.Scale());
    if (ImGui::BeginChild("##recent-entries", ImVec2(0, bodyHeight))) {
        if (m_loading.valid()) ImGui::TextWrapped("%s", Tr(m_language, TextId::RecentCapturesLoading));
        else if (m_snapshot.entries.empty()) {
            ImGui::TextWrapped("%s", Tr(m_language, TextId::RecentCapturesEmpty));
            ImGuiUtils::MutedWrappedText(Tr(m_language, TextId::HistoryEmpty));
        }
        for (size_t i = 0; i < m_snapshot.entries.size(); ++i) RenderEntry(i);
    }
    ImGui::EndChild();
    RenderFooter();
}

void RecentCapturesWindow::RenderEntry(size_t index) {
    const auto& entry = m_snapshot.entries[index];
    const auto& colors = GetHistoryPalette(m_theme);
    const float scale = m_surface.Scale();
    const auto position = ImGui::GetCursorScreenPos();
    const float height = RowHeight * scale - ImGui::GetStyle().ItemSpacing.y;
    const ImRect row(position, ImVec2(position.x + ImGui::GetContentRegionAvail().x, position.y + height));
    ImGui::PushID(entry.metadata.id.c_str());
    const bool clicked = ImGui::InvisibleButton("##recent-row", row.GetSize(), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    const bool hovered = ImGui::IsItemHovered();
    if (int(index) == m_selected && m_scrollToSelection) { ImGui::SetScrollHereY(); m_scrollToSelection = false; }
    if (hovered && m_mouseMoved && !m_busy) { Select(int(index)); m_previewEnabled = true; }
    auto* draw = ImGui::GetWindowDrawList();
    if (hovered || int(index) == m_selected) draw->AddRectFilled(row.Min, row.Max, colors.selected, RowRounding * scale);
    const ImVec2 imageStart(position.x + RowPadding * scale, position.y + (height - ThumbnailHeight * scale) * .5f);
    const ImRect imageBounds(imageStart, ImVec2(imageStart.x + ThumbnailWidth * scale, imageStart.y + ThumbnailHeight * scale));
    ImGuiUtils::DrawImageContained(draw, (ImTextureID)(intptr_t)entry.texture,
        ImVec2(static_cast<float>(entry.thumbnail.width), static_cast<float>(entry.thumbnail.height)), imageBounds, colors.border);
    const ImVec2 namePosition(position.x + TextInset * scale, position.y + TextTop * scale);
    const ImVec2 textMaximum(row.Max.x - RowPadding * scale, row.Max.y);
    const auto name = HistoryName(entry.metadata, m_language);
    ImGuiUtils::DrawClippedText(draw, namePosition, textMaximum, colors.text, name.c_str());
    const auto details = HistoryTime(entry.metadata.createdAtUnixMs, "%H:%M") + " · " +
        (entry.metadata.details.sourceApplication.empty() ? std::format("{} × {}", entry.metadata.width, entry.metadata.height)
                                                        : entry.metadata.details.sourceApplication);
    ImGuiUtils::DrawClippedText(draw, ImVec2(namePosition.x, position.y + DetailTop * scale), textMaximum, colors.muted, details.c_str());
    if (clicked && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) Queue(Action::Copy, entry.metadata.id);
    if (ImGui::BeginPopupContextItem("##recent-actions")) {
        const std::pair<TextId, Action> actions[] = {{TextId::HistoryCopy, Action::Copy}, {TextId::HistoryEdit, Action::Edit},
            {TextId::HistorySave, Action::Save}, {TextId::HistoryPin, Action::Pin}};
        for (const auto& [label, action] : actions)
            if (ImGui::MenuItem(Tr(m_language, label))) Queue(action, entry.metadata.id);
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

void RecentCapturesWindow::RenderFooter() {
    const float scale = m_surface.Scale();
    const auto available = ImGui::GetContentRegionAvail();
    const float settingsWidth = SettingsButtonWidth * scale;
    const float historyWidth = std::max(1.0f, available.x - settingsWidth - ImGui::GetStyle().ItemSpacing.x);
    if (ImGui::Button(Tr(m_language, TextId::RecentCapturesAll), ImVec2(historyWidth, FooterButtonHeight * scale))) Queue(Action::History);
    ImGui::SameLine();
    if (ImGui::Button(Tr(m_language, TextId::SettingsTitle), ImVec2(settingsWidth, FooterButtonHeight * scale))) Queue(Action::Settings);
}

void RecentCapturesWindow::PositionPreview() {
    SDL_Rect main{};
    SDL_GetWindowPosition(m_surface.Window(), &main.x, &main.y);
    SDL_GetWindowSize(m_surface.Window(), &main.w, &main.h);
    const float scale = m_surface.Scale();
    const auto bounds = WindowUtils::PlaceAdjacentPopup(main, {int(PreviewWidth * scale), int(PreviewHeight * scale)},
        m_workArea, int(EdgeGap * scale));
    // SDL 子弹层的位置以父窗口客户区为原点，工作区计算使用全局坐标。
    SDL_SetWindowPosition(m_previewSurface.Window(), bounds.x - main.x, bounds.y - main.y);
    SDL_SetWindowSize(m_previewSurface.Window(), bounds.w, bounds.h);
}

void RecentCapturesWindow::RenderPreview() {
    if (!m_previewEnabled || m_selected < 0 || Clock::now() < m_previewDue) return;
    {
        const ImGuiUtils::ContextScope context(m_surface.Context());
        // 右键菜单拥有鼠标时暂停预览，避免弹出的图片遮挡菜单。
        if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
            m_previewSurface.Hide(); return;
        }
    }
    if (!m_previewSurface.Create(Tr(m_language, TextId::HistoryPreview), {PreviewWidth, PreviewHeight}, m_surface.Window())) return;
    PositionPreview();
    m_previewSurface.Show(false);
    StartPreview();
    m_previewSurface.Render(m_theme, [this] { RenderPreviewContent(); });
}

void RecentCapturesWindow::RenderPreviewContent() {
    const auto& entry = m_snapshot.entries[size_t(m_selected)];
    const auto name = HistoryName(entry.metadata, m_language);
    ImGui::TextUnformatted(name.c_str());
    ImGui::TextDisabled("%d × %d", entry.metadata.width, entry.metadata.height);
    const auto position = ImGui::GetCursorScreenPos();
    const auto extent = ImGui::GetContentRegionAvail();
    const ImRect bounds(position, ImVec2(position.x + extent.x, position.y + extent.y));
    const bool detailed = m_previewTexture && m_previewId == entry.metadata.id;
    const auto& image = detailed ? m_preview : entry.thumbnail;
    ImGuiUtils::DrawImageContained(ImGui::GetWindowDrawList(),
        (ImTextureID)(intptr_t)(detailed ? m_previewTexture : entry.texture),
        ImVec2(static_cast<float>(image.width), static_cast<float>(image.height)),
        bounds, GetHistoryPalette(m_theme).border);
}
} // namespace shotlet
