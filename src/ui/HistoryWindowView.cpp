#define IMGUI_DEFINE_MATH_OPERATORS
#include <algorithm>
#include <array>
#include <imgui_internal.h>
#include <map>
#include "HistoryWindow.h"
#include "ui/WindowChrome.h"
#include "utils/ImGuiUtils.h"
#if defined(SHOTLET_PLATFORM_MACOS)
#include "platform/ImageEditorWindow.h"
#endif

namespace shotlet {
    namespace {
        // 布局尺寸使用逻辑坐标，只乘界面缩放；名称带 Pixel 的实际像素值不再缩放。
        // Retina 像素密度由渲染后端处理，不能再乘进这些布局尺寸。
        namespace StyleLayout {
            constexpr float MinimumViewportPixelExtent = 1;
            constexpr float CornerRadius = 6;
            constexpr float BorderWidth = 1;
            const ImVec2 FramePadding{10, 5};
            const ImVec2 ItemSpacing{8, 8};
            constexpr float PanelCornerRadius = 8;
            const ImVec2 TooltipPadding{8, 4};
        } // namespace StyleLayout
        namespace HeaderLayout {
            constexpr float NativeTitleLeft = 88;
            constexpr float TitleLeft = 20;
            const ImVec2 SearchPosition{220, 9};
            constexpr float SearchMinWidth = 160;
            constexpr float SearchMaxWidth = 420;
            constexpr float SortWidth = 122;
            constexpr float SortButtonsGap = 12;
            constexpr float ViewButtonWidth = 34;
            constexpr float ViewButtonHeight = 32;
            constexpr float RightPadding = 8;
            constexpr float SortRightReserve = SortWidth + SortButtonsGap + 2 * ViewButtonWidth + RightPadding;
            constexpr float SearchSortGap = 16;
            constexpr float SearchRightReserve = SortRightReserve + SearchSortGap;
            constexpr float ButtonCornerRadius = 4;
            constexpr int GridRows = 2;
            constexpr int GridColumns = 2;
            const ImVec2 GridOrigin{11, 10};
            constexpr float GridCellSize = 4;
            constexpr float GridCellGap = 3;
            constexpr float GridCellStep = GridCellSize + GridCellGap;
            constexpr int ListRows = 3;
            const ImVec2 ListOrigin{10, 11};
            constexpr float ListLineLength = 14;
            constexpr float ListLineStep = 5;
        } // namespace HeaderLayout
        namespace SelectionLayout {
            constexpr float Padding = 10;
            constexpr float Gap = 6;
            constexpr float RowHeight = 28;
            constexpr float BaseHeight = 38;
            constexpr float Top = 5;
            constexpr float ButtonTextPadding = 12;
            constexpr float DoneMinWidth = 48;
            constexpr float TagInputWidth = 110;
            constexpr float CheckboxLabelLeft = 26;
            // 仅缩小绘制尺寸，保留方框中心和整行点击区域，便于操作。
            const ImVec2 CheckboxMin{2, 6};
            const ImVec2 CheckboxMax{18, 22};
            constexpr float CheckboxRounding = 2.5f;
            const ImVec2 CheckOffset{3, 4};
            constexpr float CheckSize = 10;
            constexpr float PartialMarkHalfWidth = 4;
            constexpr float PartialMarkWidth = 2;
            constexpr float CheckboxStroke = 1.5f;
            constexpr size_t MinimumStitchCount = 2;
            constexpr size_t NormalActionCount = 4;
            const ImVec2 FramePadding{6, 3};
        } // namespace SelectionLayout
        namespace SidebarLayout {
            constexpr float Width = 208;
            constexpr float Padding = 12;
            constexpr float NavigationWidth = Width - 2 * Padding;
            constexpr float NavigationBottomReserve = 148;
            constexpr float NavigationRowHeight = 32;
            const ImVec2 NavigationTextOffset{8, 6};
            const ImVec2 NavigationTextMaximum{140, 30};
            constexpr float NavigationCountRight = 174;
            constexpr float SectionGap = 8;
            constexpr float TrashBottom = 128;
            // 分割线沿用一个实际像素的内缩，避免重复乘界面缩放。
            constexpr float DividerPixelInset = 1;
            constexpr float QuotaSidePadding = 10;
            constexpr float QuotaTopFromBottom = 82;
            constexpr float QuotaBottomPadding = 12;
            constexpr float QuotaTextLeft = 20;
            constexpr float QuotaTextFromBottom = 74;
            constexpr float RecordCountFromBottom = 51;
            const ImVec2 QuotaBarOffset{10, 57};
            constexpr float QuotaBarWidth = 166;
            constexpr float QuotaBarHeight = 4;
            constexpr float QuotaBarRounding = 2;
        } // namespace SidebarLayout
        namespace TagLayout {
            const ImVec2 Padding{8, 2};
            constexpr float BadgeOverflow = 5;
            const ImVec2 BadgeCenterOffset{-1, 1};
            constexpr float RemoveRadius = 6;
            constexpr float CrossHalfSize = 2.5f;
            constexpr float CrossStroke = 1.5f;
            constexpr float ContextInputWidth = 200;
        } // namespace TagLayout
        namespace FavoriteLayout {
            constexpr size_t PointCount = 10;
            constexpr float OuterRadius = 9;
            constexpr float InnerRadius = 4.2f;
            constexpr float TopAngle = -IM_PI * .5f;
            constexpr float AngleStep = 2 * IM_PI / PointCount;
            constexpr float CornerRadius = .65f;
            constexpr int CurveSegments = 3;
            constexpr float StrokeWidth = 1.6f;
        } // namespace FavoriteLayout
        namespace CardLayout {
            constexpr float PreviewPadding = 2;
            constexpr float FooterHeight = 32;
            constexpr float ListPreviewRight = 100;
            constexpr float FavoriteRight = 16;
            constexpr float FavoriteTop = 18;
            constexpr float FavoriteHitRadius = 12;
            constexpr float FavoriteButtonSize = 2 * FavoriteHitRadius;
            constexpr float BadgePadding = 8;
            constexpr float SelectedBadgeLeft = 32;
            const ImVec2 BadgeTextPadding{3, 2};
            constexpr float BadgeRounding = 3;
            const ImVec2 ListTitleOffset{116, 10};
            constexpr float ListTextRightPadding = 12;
            constexpr float ListTitleBottom = 35;
            constexpr float ListDescriptionTop = 32;
            constexpr float ListDescriptionBottom = 50;
            constexpr float FooterPadding = 10;
            constexpr float FooterTextBottom = 25;
            constexpr float SelectionCenterX = 14;
            constexpr float SelectionRadius = 8;
            const ImVec2 CheckStart{-3, 0};
            const ImVec2 CheckMiddle{-1, 3};
            const ImVec2 CheckEnd{4, -4};
            constexpr float CheckStroke = 1.5f;
            constexpr float SelectedBorderWidth = 2;
            constexpr float BorderWidth = 1;
            constexpr float ActionBarHeight = 34;
            constexpr float ListActionRounding = 6;
            constexpr float ActionTextPadding = 2;
            constexpr float ActionSeparatorWidth = 10;
            constexpr float ActionSideReserve = 16;
        } // namespace CardLayout
        namespace BrowserLayout {
            constexpr float Padding = 20;
            constexpr float CardsGap = 14;
            constexpr float MinimumCardWidth = 176;
            constexpr float GridCardHeight = 148;
            constexpr float ListCardHeight = 86;
            constexpr float DateGroupGap = 10;
            constexpr float SelectionTop = 12;
            constexpr float EmptyMinTop = 80;
            constexpr float EmptyTopFraction = .25f;
            const ImVec2 EmptyIconHalfSize{34, 24};
            constexpr float EmptyIconStroke = 2;
            constexpr float EmptyTitleTop = 72;
            constexpr float EmptyDescriptionTop = 110;
            constexpr float EmptyDescriptionWidth = 300;
            constexpr float EmptyButtonTop = 175;
            const ImVec2 EmptyButtonSize{140, 34};
        } // namespace BrowserLayout
        namespace DetailsLayout {
            constexpr float Width = 280;
            constexpr float Padding = 16;
            constexpr float ActionsReserve = 124;
            constexpr float PreviewHeight = 150;
            constexpr float PreviewInset = 6;
            constexpr float TitleGap = 4;
            constexpr float FillRemainingWidth = -1;
            constexpr float FieldLabelWidth = 64;
            constexpr float OcrTopGap = 8;
            constexpr float OcrHeight = 110;
            constexpr float ActionsFromBottom = 90;
            constexpr float ActionButtonHeight = 32;
            constexpr float ActionGap = 8;
            constexpr float ZoomSliderWidth = 200;
            constexpr float MinimumZoom = .1f;
            constexpr float MaximumZoom = 4;
            const ImVec2 ZoomPreviewSize{600, 400};
        } // namespace DetailsLayout
        namespace DialogLayout {
            constexpr float Width = 440;
            constexpr float ContentWidth = 410;
            constexpr float OrderHeight = 200;
        } // namespace DialogLayout
        namespace ToastLayout {
            constexpr float BottomPadding = 24;
            constexpr float BackgroundAlpha = .96f;
            const ImVec2 Padding{16, 10};
        } // namespace ToastLayout
        namespace TextLayout {
            constexpr size_t CountCapacity = 24;
            constexpr size_t DescriptionCapacity = 96;
            constexpr uint64_t BytesPerKibibyte = 1024;
            constexpr uint64_t BytesPerMebibyte = BytesPerKibibyte * BytesPerKibibyte;
            constexpr const char *DateFormat = "%Y-%m-%d";
            constexpr const char *DateTimeFormat = "%Y-%m-%d %H:%M";
            constexpr const char *TimeFormat = "%H:%M";
        } // namespace TextLayout
        TextId TypeLabel(const std::string &type) {
            if (type == "region")
                return TextId::HistoryRegion;
            if (type == "window")
                return TextId::HistoryWindowType;
            if (type == "fullscreen")
                return TextId::HistoryFullscreen;
            if (type == "scroll")
                return TextId::HistoryScroll;
            return TextId::HistoryScreenshot;
        }
        void DrawFavoriteIcon(ImDrawList *draw, ImVec2 center, float scale, ImU32 color, bool filled, bool hovered) {
            std::array<ImVec2, FavoriteLayout::PointCount> points;
            for (size_t i = 0; i < points.size(); ++i) {
                const float angle = FavoriteLayout::TopAngle + i * FavoriteLayout::AngleStep;
                const float radius = (i % 2 ? FavoriteLayout::InnerRadius : FavoriteLayout::OuterRadius) * scale;
                points[i] = center + ImVec2(std::cos(angle), std::sin(angle)) * radius;
            }
            const auto flags = draw->Flags;
            // 星形不依赖字体线条纹理，空心和实心复用相同的圆润外轮廓。
            draw->Flags |= ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;
            draw->Flags &= ~ImDrawListFlags_AntiAliasedLinesUseTex;
            for (size_t i = 0; i < points.size(); ++i) {
                const auto current = points[i];
                const auto incoming = points[(i + points.size() - 1) % points.size()] - current;
                const auto outgoing = points[(i + 1) % points.size()] - current;
                const float rounding = FavoriteLayout::CornerRadius * scale;
                draw->PathLineTo(current + incoming * (rounding / std::sqrt(ImLengthSqr(incoming))));
                draw->PathBezierQuadraticCurveTo(current,
                                                 current + outgoing * (rounding / std::sqrt(ImLengthSqr(outgoing))),
                                                 FavoriteLayout::CurveSegments);
            }
            if (filled)
                draw->AddConcavePolyFilled(draw->_Path.Data, draw->_Path.Size, color);
            // 只替换已有描边的颜色，复用原路径和线宽，确保悬停时图标不会变大。
            draw->PathStroke(filled && hovered ? Colors::White : color, ImDrawFlags_Closed,
                             FavoriteLayout::StrokeWidth * scale);
            draw->Flags = flags;
        }
        void DrawViewModeIcon(ImDrawList *draw, ImVec2 position, float scale, ImU32 color, bool list) {
            if (list) {
                for (int row = 0; row < HeaderLayout::ListRows; ++row) {
                    const auto start =
                            position + (HeaderLayout::ListOrigin + ImVec2(0, row * HeaderLayout::ListLineStep)) * scale;
                    draw->AddLine(start, start + ImVec2(HeaderLayout::ListLineLength * scale, 0), color);
                }
                return;
            }
            for (int row = 0; row < HeaderLayout::GridRows; ++row) {
                for (int column = 0; column < HeaderLayout::GridColumns; ++column) {
                    const auto start =
                            position +
                            (HeaderLayout::GridOrigin + ImVec2(float(column), float(row)) * HeaderLayout::GridCellStep) *
                                    scale;
                    draw->AddRect(start, start + ImVec2(HeaderLayout::GridCellSize, HeaderLayout::GridCellSize) * scale,
                                  color);
                }
            }
        }
    } // namespace

    struct HistoryWindow::SelectionHeaderLayout {
        float width = 0, height = 0;
        size_t count = 0;
        bool allSelected = false, trash = false, wrap = false;
        const char *selectAllLabel = nullptr;
        std::array<char, TextLayout::DescriptionCapacity> progress{};
        float checkboxWidth = 0, doneWidth = 0, clearWidth = 0, rightWidth = 0, actionsWidth = 0, tagWidth = 0;
        float rightX = 0, actionsX = 0, actionsY = 0;
    };

    void HistoryWindow::ApplyStyle() {
        auto &style = ImGui::GetStyle();
        style = ImGuiStyle();
        ApplyImGuiTheme(style, m_theme);
        const auto &colors = GetHistoryPalette(m_theme);
        style.WindowRounding = style.ChildRounding = style.FrameRounding = style.PopupRounding =
                StyleLayout::CornerRadius;
        style.FrameBorderSize = style.WindowBorderSize = StyleLayout::BorderWidth;
        style.FramePadding = StyleLayout::FramePadding;
        style.ItemSpacing = StyleLayout::ItemSpacing;
        const auto set = [&](ImGuiCol index, ImU32 color) {
            style.Colors[index] = ImGui::ColorConvertU32ToFloat4(color);
        };
        set(ImGuiCol_WindowBg, colors.background);
        set(ImGuiCol_ChildBg, colors.background);
        set(ImGuiCol_PopupBg, colors.sidebar);
        set(ImGuiCol_Border, colors.border);
        set(ImGuiCol_Text, colors.text);
        set(ImGuiCol_TextDisabled, colors.muted);
        set(ImGuiCol_FrameBg, colors.background);
        set(ImGuiCol_Button, colors.background);
        set(ImGuiCol_FrameBgHovered, colors.selected);
        set(ImGuiCol_ButtonHovered, colors.selected);
        set(ImGuiCol_Header, colors.selected);
        set(ImGuiCol_HeaderHovered, colors.selected);
        set(ImGuiCol_HeaderActive, colors.accent);
        set(ImGuiCol_ButtonActive, colors.accent);
        set(ImGuiCol_CheckMark, colors.accent);
        set(ImGuiCol_SliderGrab, colors.accent);
        set(ImGuiCol_NavCursor, colors.link);
        set(ImGuiCol_TextSelectedBg, colors.selected);
        style.ScaleAllSizes(m_uiScale);
        style.FontScaleDpi = m_uiScale;
    }

    bool HistoryWindow::RenderTagInput(float width) {
        m_tagEditorVisible = true;
        if (m_focusTagInput) {
            ImGui::SetKeyboardFocusHere();
            m_focusTagInput = false;
        }
        ImGui::SetNextItemWidth(width);
        const bool enter =
                ImGui::InputTextWithHint("##new_history_tag", Tr(m_language, TextId::HistoryTagHint), m_tagInput,
                                         sizeof(m_tagInput), ImGuiInputTextFlags_EnterReturnsTrue);
        if (!enter && !ImGui::IsItemDeactivated())
            return false;
        FinishTagEdit(!ImGui::IsKeyPressed(ImGuiKey_Escape));
        return true;
    }
    void HistoryWindow::RenderTag(const HistoryMetadata &record, const std::string &tag, float width) {
        const auto &colors = GetHistoryPalette(m_theme);
        const float scale = m_uiScale;
        const auto position = ImGui::GetCursorScreenPos();
        const auto padding = TagLayout::Padding * scale;
        const bool removable = !record.details.deletedAtUnixMs;
        const ImVec2 size(std::min(width - (removable ? TagLayout::BadgeOverflow * scale : 0),
                                   ImGui::CalcTextSize(tag.c_str()).x + 2 * padding.x),
                          ImGui::GetTextLineHeight() + 2 * padding.y);
        const ImRect body(position, position + size);
        const auto center = ImVec2(body.Max.x, body.Min.y) + TagLayout::BadgeCenterOffset * scale;
        const ImVec2 removeRadius(TagLayout::RemoveRadius * scale, TagLayout::RemoveRadius * scale);
        const ImRect removeRect(center - removeRadius, center + removeRadius);
        ImRect hitRect = body;
        if (removable)
            hitRect.Add(removeRect);
        ImGui::PushID(tag.c_str());
        const auto id = ImGui::GetID("##history_tag");
        // 外侧删除角标参与命中，但不扩大行内标签的布局尺寸。
        ImGui::ItemSize(size);
        if (!ImGui::ItemAdd(hitRect, id, &body)) {
            ImGui::PopID();
            return;
        }
        const bool clicked = ImGui::ButtonBehavior(hitRect, id, nullptr, nullptr);
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_NoNavOverride);
        const bool removeHovered = hovered && removable && removeRect.Contains(ImGui::GetIO().MousePos);
        auto *draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(position, position + size, hovered ? colors.selected : colors.panel, size.y * .5f);
        draw->AddRect(position, position + size, colors.border, size.y * .5f);
        ImGuiUtils::DrawClippedText(draw, position + padding, body.Max - padding, colors.text, tag.c_str());
        ImGui::RenderNavCursor(body, id);
        if (hovered && removable) {
            draw->AddCircleFilled(center, TagLayout::RemoveRadius * scale,
                                  ImGui::GetColorU32(GetThemePalette(m_theme).danger));
            const float cross = TagLayout::CrossHalfSize * scale;
            draw->AddLine(center - ImVec2(cross, cross), center + ImVec2(cross, cross), Colors::White,
                          TagLayout::CrossStroke * scale);
            draw->AddLine(center + ImVec2(-cross, cross), center + ImVec2(cross, -cross), Colors::White,
                          TagLayout::CrossStroke * scale);
        }
        if (clicked && removeHovered)
            Queue(Action::RemoveTag, record.id, {}, tag);
        else if (clicked) {
            m_query.scope = HistoryScope::Tag;
            m_query.value = tag;
            UpdateFilter(true);
        }
        ImGui::PopID();
    }

    void HistoryWindow::RenderHeader(float width) {
        m_headerControlRects.clear();
        const auto &colors = GetHistoryPalette(m_theme);
        const float scale = m_uiScale;
        auto *draw = ImGui::GetWindowDrawList();
        const auto origin = ImGui::GetWindowPos();
        const ImVec2 bottom(origin.x + width, origin.y + HeaderLogicalHeight * scale);
        draw->AddRectFilled(origin, bottom, colors.sidebar);
        draw->AddLine(ImVec2(origin.x, bottom.y), bottom, colors.border);
#if defined(SHOTLET_PLATFORM_MACOS)
        // AppKit 可能重新布局原生窗口按钮，随自绘顶栏同步位置。
        LayoutHistoryBrowserWindow(m_window, HeaderLogicalHeight * scale);
        const float titleLeft = HeaderLayout::NativeTitleLeft;
#else
        const float titleLeft = HeaderLayout::TitleLeft;
#endif
#if defined(SHOTLET_PLATFORM_WINDOWS)
        // 右上角共享窗口控制组常驻置顶，排序与视图按钮组、搜索框右边界均左移避开。
        const float chromeReserve = WindowChrome::WindowControlsWidth(scale, /*includeMaximize=*/true);
#else
        // macOS 交通灯按钮在左上角，右侧无需预留。
        const float chromeReserve = 0;
#endif
        ImGui::SetCursorPos(
                ImVec2(titleLeft * scale, (HeaderLogicalHeight * scale - ImGui::GetTextLineHeight()) * .5f));
        ImGui::TextUnformatted(Tr(m_language, TextId::HistoryTitle));
        ImGui::SetCursorPos(HeaderLayout::SearchPosition * scale);
        ImGui::SetNextItemWidth(std::min(
                HeaderLayout::SearchMaxWidth * scale,
                std::max(HeaderLayout::SearchMinWidth * scale,
                         width - HeaderLayout::SearchPosition.x * scale - HeaderLayout::SearchRightReserve * scale -
                                 chromeReserve)));
        if (ImGuiUtils::InputTextWithSearchIcon("##history_search", Tr(m_language, TextId::HistorySearchHint), m_search,
                                                sizeof(m_search), scale))
            UpdateFilter(true);
        m_headerControlRects.emplace_back(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        // 右侧组包含按钮间距，避免加间距后侵占窗口右侧留白。
        ImGui::SetCursorPos(ImVec2(width - HeaderLayout::SortRightReserve * scale - chromeReserve -
                                           ImGui::GetStyle().ItemSpacing.x,
                                   HeaderLayout::SearchPosition.y * scale));
        RenderSortCombo();
        m_headerControlRects.emplace_back(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
        ImGui::SameLine(0, HeaderLayout::SortButtonsGap * scale);
        RenderViewModeButtons();
    }
    void HistoryWindow::RenderSortCombo() {
        constexpr auto labels = std::to_array<TextId>(
                {TextId::HistoryNewest, TextId::HistoryOldest, TextId::HistoryNameOrder, TextId::HistoryLargest});
        ImGui::SetNextItemWidth(HeaderLayout::SortWidth * m_uiScale);
        if (!ImGui::BeginCombo("##history_sort", Tr(m_language, labels[int(m_query.sort)])))
            return;
        for (size_t i = 0; i < labels.size(); ++i) {
            if (ImGui::Selectable(Tr(m_language, labels[i]), int(m_query.sort) == int(i))) {
                m_query.sort = HistorySort(i);
                UpdateFilter();
            }
        }
        ImGui::EndCombo();
    }
    void HistoryWindow::RenderViewModeButtons() {
        const auto &colors = GetHistoryPalette(m_theme);
        const ImVec2 size(HeaderLayout::ViewButtonWidth * m_uiScale, HeaderLayout::ViewButtonHeight * m_uiScale);
        for (bool list: {false, true}) {
            if (list)
                ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x);
            const auto position = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton(list ? "history_list_view" : "history_grid_view", size)) {
                m_listView = list;
                m_resetBrowserScroll = true;
            }
            m_headerControlRects.emplace_back(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
            auto *draw = ImGui::GetWindowDrawList();
            draw->AddRect(position, position + size, colors.border, HeaderLayout::ButtonCornerRadius * m_uiScale);
            DrawViewModeIcon(draw, position, m_uiScale, m_listView == list ? colors.link : colors.muted, list);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("%s", Tr(m_language, list ? TextId::HistoryList : TextId::HistoryGrid));
        }
    }

    HistoryWindow::SelectionHeaderLayout HistoryWindow::CalculateSelectionHeaderLayout(float width) const {
        SelectionHeaderLayout layout;
        layout.width = width;
        layout.count = m_multiSelect ? m_selection.size() : 0;
        layout.allSelected = layout.count > 0 && layout.count == m_visible.size();
        layout.trash = m_query.scope == HistoryScope::Trash;
        layout.selectAllLabel =
                Tr(m_language, layout.allSelected ? TextId::HistoryDeselectAll : TextId::HistorySelectAll);
        const float scale = m_uiScale, padding = SelectionLayout::Padding * scale, gap = SelectionLayout::Gap * scale;
        const auto buttonWidth = [&](TextId label) {
            return ImGui::CalcTextSize(Tr(m_language, label)).x + SelectionLayout::ButtonTextPadding * scale;
        };
        layout.checkboxWidth =
                SelectionLayout::CheckboxLabelLeft * scale + ImGui::CalcTextSize(layout.selectAllLabel).x;
        layout.doneWidth = std::max(SelectionLayout::DoneMinWidth * scale, buttonWidth(TextId::HistoryDone));
        layout.clearWidth = buttonWidth(TextId::HistoryClearSelection);
        layout.rightWidth = m_multiSelect ? layout.doneWidth + (layout.count ? layout.clearWidth + gap : 0) : 0;
        layout.tagWidth = m_tagEditor == TagEditor::SelectionHeader ? SelectionLayout::TagInputWidth * scale
                                                                    : buttonWidth(TextId::HistoryTags);
        if (layout.count) {
            layout.actionsWidth =
                    layout.trash ? buttonWidth(TextId::HistoryRestore) + gap + buttonWidth(TextId::HistoryPurge)
                                 : buttonWidth(TextId::HistoryStitch) + buttonWidth(TextId::HistoryExport) +
                                           layout.tagWidth + buttonWidth(TextId::HistoryDelete) +
                                           (SelectionLayout::NormalActionCount - 1) * gap;
        }
        auto &progress = layout.progress;
        if (layout.count)
            std::snprintf(progress.data(), progress.size(), Tr(m_language, TextId::HistorySelectionProgress),
                          layout.count, m_visible.size());
        else
            std::snprintf(progress.data(), progress.size(), "%s", Tr(m_language, TextId::HistoryMultiSelectHint));
        const float controlsWidth = 2 * padding + layout.checkboxWidth + layout.rightWidth + (m_multiSelect ? gap : 0);
        if (layout.count &&
            controlsWidth + gap + ImGui::CalcTextSize(progress.data()).x + gap + layout.actionsWidth > width)
            std::snprintf(progress.data(), progress.size(), "%zu / %zu", layout.count, m_visible.size());
        // 先使用紧凑进度文字；仍放不下时只把批量操作移到第二行。
        layout.wrap = layout.count &&
                      controlsWidth + gap + ImGui::CalcTextSize(progress.data()).x + gap + layout.actionsWidth > width;
        layout.height = SelectionLayout::BaseHeight * scale +
                        (layout.wrap ? (SelectionLayout::RowHeight + SelectionLayout::Gap) * scale : 0);
        layout.rightX = width - padding - layout.rightWidth;
        layout.actionsX = layout.wrap ? padding : layout.rightX - gap - layout.actionsWidth;
        layout.actionsY =
                (SelectionLayout::Top + (layout.wrap ? SelectionLayout::RowHeight + SelectionLayout::Gap : 0)) * scale;
        return layout;
    }
    float HistoryWindow::RenderSelectionHeader(float width) {
        const auto layout = CalculateSelectionHeaderLayout(width);
        const float scale = m_uiScale;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, GetHistoryPalette(m_theme).panel);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, StyleLayout::PanelCornerRadius * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(SelectionLayout::Padding, SelectionLayout::Top) * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, SelectionLayout::FramePadding * scale);
        ImGui::BeginChild("history_selection_header", ImVec2(width, layout.height),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::SetCursorPos(ImVec2(SelectionLayout::Padding, SelectionLayout::Top) * scale);
        const auto origin = ImGui::GetCursorScreenPos();
        RenderSelectAllControl(layout);
        const float gap = SelectionLayout::Gap * scale;
        ImVec2 textPosition = origin + ImVec2(layout.checkboxWidth + gap,
                                              (SelectionLayout::RowHeight * scale - ImGui::GetFontSize()) * .5f);
        const ImVec2 textMaximum(ImGui::GetWindowPos().x + layout.rightX - gap,
                                 origin.y + SelectionLayout::RowHeight * scale);
        if (!layout.count) {
            // 空选中状态以整栏为中心，窄窗口下为两侧控件留出空间，避免提示覆盖按钮。
            const float textWidth = ImGui::CalcTextSize(layout.progress.data()).x;
            const float centeredX = ImGui::GetWindowPos().x + (width - textWidth) * .5f;
            textPosition.x = std::clamp(centeredX, textPosition.x, std::max(textPosition.x, textMaximum.x - textWidth));
        }
        ImGuiUtils::DrawClippedText(ImGui::GetWindowDrawList(), textPosition, textMaximum,
                                    GetHistoryPalette(m_theme).muted, layout.progress.data());
        if (layout.count)
            RenderSelectionActions(layout);
        if (m_multiSelect)
            RenderSelectionFinishControls(layout);
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor();
        return layout.height;
    }
    void HistoryWindow::RenderSelectAllControl(const SelectionHeaderLayout &layout) {
        const float scale = m_uiScale;
        const auto &colors = GetHistoryPalette(m_theme);
        const auto position = ImGui::GetCursorScreenPos();
        ImGui::BeginDisabled(m_visible.empty());
        if (ImGui::InvisibleButton("##history_select_all",
                                   ImVec2(layout.checkboxWidth, SelectionLayout::RowHeight * scale),
                                   ImGuiButtonFlags_EnableNav))
            SelectAllVisible(!layout.allSelected);
        const auto checkbox = ImGui::GetItemRectMin();
        auto *draw = ImGui::GetWindowDrawList();
        const ImRect box(checkbox + SelectionLayout::CheckboxMin * scale,
                         checkbox + SelectionLayout::CheckboxMax * scale);
        if (layout.count) {
            draw->AddRectFilled(box.Min, box.Max, colors.accent, SelectionLayout::CheckboxRounding * scale);
            if (layout.allSelected)
                ImGui::RenderCheckMark(draw, box.Min + SelectionLayout::CheckOffset * scale, Colors::White,
                                       SelectionLayout::CheckSize * scale);
            else
                draw->AddLine(box.GetCenter() - ImVec2(SelectionLayout::PartialMarkHalfWidth * scale, 0),
                              box.GetCenter() + ImVec2(SelectionLayout::PartialMarkHalfWidth * scale, 0),
                              colors.background, SelectionLayout::PartialMarkWidth * scale);
        } else
            draw->AddRect(box.Min, box.Max, colors.border, SelectionLayout::CheckboxRounding * scale, ImDrawFlags_None,
                          SelectionLayout::CheckboxStroke * scale);
        draw->AddText(position + ImVec2(SelectionLayout::CheckboxLabelLeft * scale,
                                        (SelectionLayout::RowHeight * scale - ImGui::GetFontSize()) * .5f),
                      m_visible.empty() ? colors.muted : colors.text, layout.selectAllLabel);
        ImGui::RenderNavCursor(ImGui::GetCurrentContext()->LastItemData.Rect, ImGui::GetItemID());
        ImGui::EndDisabled();
    }
    void HistoryWindow::RenderSelectionActions(const SelectionHeaderLayout &layout) {
        const float gap = SelectionLayout::Gap * m_uiScale;
        ImGui::SetCursorPos(ImVec2(layout.actionsX, layout.actionsY));
        const auto button = [&](TextId label) {
            const auto text = Tr(m_language, label);
            const ImVec2 size(ImGui::CalcTextSize(text).x + SelectionLayout::ButtonTextPadding * m_uiScale,
                              SelectionLayout::RowHeight * m_uiScale);
            if (label == TextId::HistoryDelete || label == TextId::HistoryPurge)
                return ImGuiUtils::DestructiveButton(text, GetThemePalette(m_theme), size);
            return ImGui::Button(text, size);
        };
        ImGui::BeginDisabled(m_busy);
        if (layout.trash) {
            if (button(TextId::HistoryRestore))
                Queue(Action::Restore, {}, SelectedIds());
            ImGui::SameLine(0, gap);
            if (button(TextId::HistoryPurge))
                Confirm(Action::Purge, SelectedIds());
        } else {
            ImGui::BeginDisabled(layout.count < SelectionLayout::MinimumStitchCount);
            if (button(TextId::HistoryStitch))
                OpenStitch();
            ImGui::EndDisabled();
            ImGui::SameLine(0, gap);
            if (button(TextId::HistoryExport))
                Queue(Action::Export, {}, SelectedIds());
            ImGui::SameLine(0, gap);
            if (m_tagEditor == TagEditor::SelectionHeader)
                RenderTagInput(layout.tagWidth);
            else if (button(TextId::HistoryTags))
                StartTagEdit(SelectedIds(), TagEditor::SelectionHeader);
            ImGui::SameLine(0, gap);
            if (button(TextId::HistoryDelete))
                Queue(Action::Delete, {}, SelectedIds());
        }
        ImGui::EndDisabled();
    }
    void HistoryWindow::RenderSelectionFinishControls(const SelectionHeaderLayout &layout) {
        const float scale = m_uiScale;
        if (layout.count) {
            ImGui::SetCursorPos(ImVec2(layout.rightX, SelectionLayout::Top * scale));
            if (ImGui::Button(Tr(m_language, TextId::HistoryClearSelection),
                              ImVec2(layout.clearWidth, SelectionLayout::RowHeight * scale)))
                SelectAllVisible(false);
        }
        ImGui::SetCursorPos(ImVec2(layout.width - SelectionLayout::Padding * scale - layout.doneWidth,
                                   SelectionLayout::Top * scale));
        if (ImGuiUtils::PrimaryButton(Tr(m_language, TextId::HistoryDone),
                                      ImVec2(layout.doneWidth, SelectionLayout::RowHeight * scale),
                                      GetHistoryPalette(m_theme).accent))
            EndMultiSelect();
    }

    void HistoryWindow::RenderSidebar(float height) {
        const auto &colors = GetHistoryPalette(m_theme);
        const float scale = m_uiScale;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.sidebar);
        ImGui::BeginChild("history_sidebar", ImVec2(SidebarLayout::Width * scale, height), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar);
        const auto start = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine(
                ImVec2(start.x + SidebarLayout::Width * scale - SidebarLayout::DividerPixelInset, start.y),
                ImVec2(start.x + SidebarLayout::Width * scale - SidebarLayout::DividerPixelInset, start.y + height),
                colors.border);
        ImGui::SetCursorPos(ImVec2(SidebarLayout::Padding, SidebarLayout::Padding) * scale);
        ImGui::BeginChild("history_navigation", ImVec2(SidebarLayout::NavigationWidth * scale,
                                                       height - SidebarLayout::NavigationBottomReserve * scale));
        const auto section = [&](TextId label) {
            ImGui::Dummy(ImVec2(0, SidebarLayout::SectionGap * scale));
            ImGui::TextDisabled("%s", Tr(m_language, label));
        };
        RenderNavigationItem(HistoryScope::All, TextId::HistoryAll);
        RenderNavigationItem(HistoryScope::Favorites, TextId::HistoryFavorites);
        section(TextId::HistoryTimeSection);
        RenderNavigationItem(HistoryScope::Today, TextId::HistoryToday);
        RenderNavigationItem(HistoryScope::Yesterday, TextId::HistoryYesterday);
        RenderNavigationItem(HistoryScope::Week, TextId::HistoryWeek);
        section(TextId::HistoryTypes);
        RenderNavigationItem(HistoryScope::Type, TextId::HistoryRegion, "region");
        RenderNavigationItem(HistoryScope::Type, TextId::HistoryWindowType, "window");
        RenderNavigationItem(HistoryScope::Type, TextId::HistoryFullscreen, "fullscreen");
        RenderNavigationItem(HistoryScope::Type, TextId::HistoryScroll, "scroll");
        section(TextId::HistoryTags);
        for (const auto &tag: AvailableTags())
            RenderNavigationItem(HistoryScope::Tag, TextId::HistoryTags, tag, tag.c_str());
        ImGui::EndChild();
        ImGui::SetCursorPos(ImVec2(SidebarLayout::Padding * scale, height - SidebarLayout::TrashBottom * scale));
        RenderNavigationItem(HistoryScope::Trash, TextId::HistoryTrash);
        RenderStorageQuota(height);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    void HistoryWindow::RenderNavigationItem(HistoryScope scope, TextId label, const std::string &value,
                                             const char *custom) {
        const auto &colors = GetHistoryPalette(m_theme);
        const float scale = m_uiScale;
        HistoryQuery query;
        query.scope = scope;
        query.value = value;
        const auto count = std::count_if(m_records.begin(), m_records.end(), [&](const auto &record) {
            return HistoryMatches(record, query, HistoryNow());
        });
        ImGui::PushID(int(scope));
        ImGui::PushID(value.c_str());
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(StyleLayout::ItemSpacing.x * scale, 0));
        const auto position = ImGui::GetCursorScreenPos();
        const bool selected = m_query.scope == scope && m_query.value == value;
        // 选中项保持深绿底和白字，悬停未选中项时使用浅绿底。
        ImGui::PushStyleColor(ImGuiCol_Header, colors.accent);
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, selected ? colors.accent : colors.selected);
        const bool clicked = ImGui::Selectable("##navigation", selected, ImGuiSelectableFlags_None,
                                               ImVec2(0, SidebarLayout::NavigationRowHeight * scale));
        ImGui::PopStyleColor(2);
        if (clicked) {
            m_query.scope = scope;
            m_query.value = value;
            UpdateFilter(true);
        }
        auto *draw = ImGui::GetWindowDrawList();
        char number[TextLayout::CountCapacity];
        std::snprintf(number, sizeof(number), "%zu", size_t(count));
        ImGuiUtils::DrawClippedText(draw, position + SidebarLayout::NavigationTextOffset * scale,
                                    position + SidebarLayout::NavigationTextMaximum * scale,
                                    selected ? Colors::White : colors.text, custom ? custom : Tr(m_language, label));
        draw->AddText(position + ImVec2(SidebarLayout::NavigationCountRight * scale - ImGui::CalcTextSize(number).x,
                                        SidebarLayout::NavigationTextOffset.y * scale),
                      selected ? Colors::White : colors.muted, number);
        ImGui::PopStyleVar();
        ImGui::PopID();
        ImGui::PopID();
    }
    void HistoryWindow::RenderStorageQuota(float height) {
        const auto &colors = GetHistoryPalette(m_theme);
        const float scale = m_uiScale;
        const auto start = ImGui::GetWindowPos();
        auto *draw = ImGui::GetWindowDrawList();
        const ImVec2 position(start.x + SidebarLayout::QuotaSidePadding * scale,
                              start.y + height - SidebarLayout::QuotaTopFromBottom * scale);
        draw->AddRect(position,
                      ImVec2(start.x + (SidebarLayout::Width - SidebarLayout::QuotaSidePadding) * scale,
                             start.y + height - SidebarLayout::QuotaBottomPadding * scale),
                      colors.border, StyleLayout::PanelCornerRadius * scale);
        ImGui::SetCursorPos(
                ImVec2(SidebarLayout::QuotaTextLeft * scale, height - SidebarLayout::QuotaTextFromBottom * scale));
        ImGui::Text(Tr(m_language, TextId::HistoryQuota), double(m_totalBytes) / TextLayout::BytesPerMebibyte);
        ImGui::SetCursorPos(
                ImVec2(SidebarLayout::QuotaTextLeft * scale, height - SidebarLayout::RecordCountFromBottom * scale));
        ImGui::TextDisabled("%zu / %zu", m_records.size(), HistoryStore::DefaultMaxRecords);
        const auto bar = position + SidebarLayout::QuotaBarOffset * scale;
        draw->AddRectFilled(bar, bar + ImVec2(SidebarLayout::QuotaBarWidth, SidebarLayout::QuotaBarHeight) * scale,
                            colors.border, SidebarLayout::QuotaBarRounding * scale);
        const double usage = std::min(1.0, double(m_totalBytes) / HistoryStore::DefaultMaxBytes);
        // 用量先按 double 截断到 [0,1]，再一次性收窄为 float，避免整个条宽表达式升级为 double。
        draw->AddRectFilled(bar,
                            bar + ImVec2(SidebarLayout::QuotaBarWidth * scale * static_cast<float>(usage),
                                         SidebarLayout::QuotaBarHeight * scale),
                            colors.accent, SidebarLayout::QuotaBarRounding * scale);
    }

    void HistoryWindow::RenderContextMenu(int index) {
        const auto &record = m_records[index];
        if (!ImGui::BeginPopupContextItem("history_card_menu"))
            return;
        const auto ids =
                m_selection.contains(record.id) && m_multiSelect ? SelectedIds() : std::vector<std::string>{record.id};
        if (record.details.deletedAtUnixMs) {
            if (ImGui::MenuItem(Tr(m_language, TextId::HistoryRestore)))
                Queue(Action::Restore, {}, ids);
            if (ImGuiUtils::DestructiveMenuItem(Tr(m_language, TextId::HistoryPurge), GetThemePalette(m_theme)))
                Confirm(Action::Purge, ids);
            ImGui::EndPopup();
            return;
        }
        if (ImGui::MenuItem(Tr(m_language, TextId::HistoryCopy), "Enter"))
            Queue(Action::Copy, record.id);
        if (ImGui::MenuItem(Tr(m_language, TextId::HistoryCopyText)))
            Queue(Action::CopyText, record.id);
        if (ImGui::MenuItem(Tr(m_language, TextId::HistoryEdit)))
            Queue(Action::Edit, record.id);
        if (ImGui::MenuItem(Tr(m_language, TextId::HistoryPin)))
            Queue(Action::Pin, record.id);
        ImGui::Separator();
        RenderTagMenu(ids);
        if (ImGui::MenuItem(
                    Tr(m_language, record.details.favorite ? TextId::HistoryUnfavorite : TextId::HistoryFavorite)))
            Queue(Action::Favorite, {}, ids, {}, !record.details.favorite);
        if (ImGui::MenuItem(Tr(m_language, TextId::HistoryRename))) {
            Select(index);
            m_focusTitle = true;
        }
        if (ImGui::MenuItem(Tr(m_language, TextId::HistoryReveal)))
            Queue(Action::Reveal, record.id);
        if (ImGui::MenuItem(Tr(m_language, TextId::HistoryExport)))
            Queue(Action::Export, {}, ids);
        ImGui::Separator();
        if (ImGuiUtils::DestructiveMenuItem(Tr(m_language, TextId::HistoryDelete), GetThemePalette(m_theme), "Del"))
            Queue(Action::Delete, {}, ids);
        ImGui::EndPopup();
    }
    void HistoryWindow::RenderTagMenu(const std::vector<std::string> &ids) {
        if (!ImGui::BeginMenu(Tr(m_language, TextId::HistoryTagManage)))
            return;
        const auto tags = AvailableTags();
        for (const auto &tag: tags)
            if (ImGui::MenuItem(tag.c_str()))
                Queue(Action::AddTag, {}, ids, tag);
        if (!tags.empty())
            ImGui::Separator();
        if (m_tagEditor == TagEditor::ContextMenu && m_tagTargets == ids) {
            if (RenderTagInput(TagLayout::ContextInputWidth * m_uiScale))
                ImGui::CloseCurrentPopup();
        } else {
            ImGui::PushItemFlag(ImGuiItemFlags_AutoClosePopups, false);
            if (ImGui::MenuItem(Tr(m_language, TextId::HistoryAddTag)))
                StartTagEdit(ids, TagEditor::ContextMenu);
            ImGui::PopItemFlag();
        }
        ImGui::EndMenu();
    }

    void HistoryWindow::RenderCard(int index, ImVec2 size, bool list) {
        const auto &colors = GetHistoryPalette(m_theme);
        const float scale = m_uiScale;
        const auto &record = m_records[index];
        ImGui::PushID(record.id.c_str());
        ImGui::BeginGroup();
        const auto position = ImGui::GetCursorScreenPos();
        const ImRect bounds(position, position + size);
        const ImRect preview(
                position + ImVec2(CardLayout::PreviewPadding, CardLayout::PreviewPadding) * scale,
                ImVec2(position.x + (list ? CardLayout::ListPreviewRight * scale
                                          : size.x - CardLayout::PreviewPadding * scale),
                       position.y + size.y - (list ? CardLayout::PreviewPadding : CardLayout::FooterHeight) * scale));
        // 列表缩略图较窄，多选时把收藏按钮放到标签下方，避免遮挡同一行的圆标和类型标签。
        const float favoriteTop =
                CardLayout::FavoriteTop * scale +
                (m_multiSelect && list ? ImGui::GetTextLineHeight() + 2 * CardLayout::BadgeTextPadding.y * scale : 0);
        const ImVec2 favoriteCenter(preview.Max.x - CardLayout::FavoriteRight * scale, position.y + favoriteTop);
        const bool cardUnderMouse = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                                    ImGui::IsMouseHoveringRect(bounds.Min, bounds.Max);
        const bool showFavorite = !record.details.deletedAtUnixMs && (record.details.favorite || cardUnderMouse);
        // 收藏按钮先占用点击，防止收藏操作同时选中卡片或触发双击编辑。
        bool favoriteHovered = false;
        if (showFavorite) {
            favoriteHovered = RenderCardFavoriteButton(record, favoriteCenter);
            ImGui::SetCursorScreenPos(position);
        }
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##history_card", size,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlappedByItem |
                                                  ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
        const bool visible = ImGui::IsItemVisible();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            const auto &io = ImGui::GetIO();
            ClickRecord(index, io.KeyShift, io.KeyCtrl || io.KeySuper);
            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !m_multiSelect && !record.details.deletedAtUnixMs)
                Queue(Action::Edit, record.id);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && !m_selection.contains(record.id)) {
            m_selection = {record.id};
            Select(index);
        }
        RenderContextMenu(index);
        const bool selected = m_multiSelect ? m_selection.contains(record.id) : m_selectedId == record.id;
        auto *draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(bounds.Min, bounds.Max, colors.panel, StyleLayout::PanelCornerRadius * scale);
        RenderCardPreview(record, preview, visible);
        RenderCardCaption(record, bounds, list);
        if (showFavorite)
            DrawFavoriteIcon(draw, favoriteCenter, scale,
                             record.details.favorite || favoriteHovered
                                     ? ImGui::GetColorU32(GetThemePalette(m_theme).danger)
                                     : Colors::White,
                             record.details.favorite, favoriteHovered);
        if (m_multiSelect)
            RenderCardSelection(position, selected);
        draw->AddRect(bounds.Min, bounds.Max, selected ? colors.accent : colors.border,
                      StyleLayout::PanelCornerRadius * scale, ImDrawFlags_None,
                      (selected ? CardLayout::SelectedBorderWidth : CardLayout::BorderWidth) * scale);
        if (hovered && !m_multiSelect && !record.details.deletedAtUnixMs)
            RenderCardQuickActions(record, bounds, preview, list);
        ImGui::EndGroup();
        ImGui::PopID();
    }
    bool HistoryWindow::RenderCardFavoriteButton(const HistoryMetadata &record, ImVec2 center) {
        const float scale = m_uiScale;
        ImGui::SetCursorScreenPos(center -
                                  ImVec2(CardLayout::FavoriteHitRadius, CardLayout::FavoriteHitRadius) * scale);
        if (ImGui::InvisibleButton("##history_favorite",
                                   ImVec2(CardLayout::FavoriteButtonSize, CardLayout::FavoriteButtonSize) * scale,
                                   ImGuiButtonFlags_EnableNav))
            Queue(Action::Favorite, record.id, {}, {}, !record.details.favorite);
        // 在提示浮层或卡片控件覆盖 LastItemData 之前记录悬停状态。
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, StyleLayout::TooltipPadding * scale);
        ImGui::SetItemTooltip(
                "%s", Tr(m_language, record.details.favorite ? TextId::HistoryUnfavorite : TextId::HistoryFavorite));
        ImGui::PopStyleVar();
        return hovered;
    }
    void HistoryWindow::RenderCardPreview(const HistoryMetadata &record, const ImRect &preview, bool visible) {
        if (!visible || record.width <= 0 || record.height <= 0)
            return;
        auto *texture = Thumbnail(record.id);
        if (!texture)
            return;
        const float fit = std::min(preview.GetWidth() / record.width, preview.GetHeight() / record.height);
        const ImVec2 size(record.width * fit, record.height * fit);
        const auto minimum = preview.GetCenter() - size * .5f;
        ImGui::GetWindowDrawList()->AddImage((ImTextureID) (intptr_t) texture, minimum, minimum + size);
    }
    void HistoryWindow::RenderCardCaption(const HistoryMetadata &record, const ImRect &bounds, bool list) {
        const auto &colors = GetHistoryPalette(m_theme);
        const float scale = m_uiScale;
        const auto position = bounds.Min;
        auto *draw = ImGui::GetWindowDrawList();
        const auto badge = Tr(m_language, TypeLabel(record.details.captureType));
        const auto badgeSize = ImGui::CalcTextSize(badge);
        const auto badgePosition =
                position + ImVec2(m_multiSelect ? CardLayout::SelectedBadgeLeft : CardLayout::BadgePadding,
                                  CardLayout::BadgePadding) *
                                   scale;
        draw->AddRectFilled(badgePosition - CardLayout::BadgeTextPadding * scale,
                            badgePosition + badgeSize + CardLayout::BadgeTextPadding * scale,
                            Colors::HistoryBadgeBackground, CardLayout::BadgeRounding * scale);
        draw->AddText(badgePosition, Colors::White, badge);
        if (list) {
            ImGuiUtils::DrawClippedText(draw, position + CardLayout::ListTitleOffset * scale,
                                        ImVec2(bounds.Max.x - CardLayout::ListTextRightPadding * scale,
                                               position.y + CardLayout::ListTitleBottom * scale),
                                        colors.text, HistoryName(record, m_language).c_str());
            const auto description = HistoryTime(record.createdAtUnixMs, TextLayout::DateTimeFormat) + "   " +
                                     std::to_string(record.width) + " × " + std::to_string(record.height);
            ImGuiUtils::DrawClippedText(
                    draw, position + ImVec2(CardLayout::ListTitleOffset.x, CardLayout::ListDescriptionTop) * scale,
                    ImVec2(bounds.Max.x - CardLayout::ListTextRightPadding * scale,
                           position.y + CardLayout::ListDescriptionBottom * scale),
                    colors.muted, description.c_str());
            return;
        }
        const auto time = HistoryTime(record.createdAtUnixMs, TextLayout::TimeFormat);
        draw->AddText(ImVec2(position.x + CardLayout::FooterPadding * scale,
                             bounds.Max.y - CardLayout::FooterTextBottom * scale),
                      colors.muted, time.c_str());
        const auto dimensions = std::to_string(record.width) + "×" + std::to_string(record.height);
        draw->AddText(
                ImVec2(bounds.Max.x - CardLayout::FooterPadding * scale - ImGui::CalcTextSize(dimensions.c_str()).x,
                       bounds.Max.y - CardLayout::FooterTextBottom * scale),
                colors.muted, dimensions.c_str());
    }
    void HistoryWindow::RenderCardSelection(ImVec2 position, bool selected) {
        const auto &colors = GetHistoryPalette(m_theme);
        const float scale = m_uiScale;
        auto *draw = ImGui::GetWindowDrawList();
        // 跟随类型标签的实际行高对齐中心，字体或界面缩放变化时仍处于同一水平线。
        const ImVec2 center(position.x + CardLayout::SelectionCenterX * scale,
                            position.y + CardLayout::BadgePadding * scale + ImGui::GetTextLineHeight() * .5f);
        if (!selected) {
            draw->AddCircle(center, CardLayout::SelectionRadius * scale, colors.border, 0,
                            CardLayout::CheckStroke * scale);
            return;
        }
        draw->AddCircleFilled(center, CardLayout::SelectionRadius * scale, colors.accent);
        draw->AddLine(center + CardLayout::CheckStart * scale, center + CardLayout::CheckMiddle * scale, Colors::White,
                      CardLayout::CheckStroke * scale);
        draw->AddLine(center + CardLayout::CheckMiddle * scale, center + CardLayout::CheckEnd * scale, Colors::White,
                      CardLayout::CheckStroke * scale);
    }
    void HistoryWindow::RenderCardQuickActions(const HistoryMetadata &record, const ImRect &bounds,
                                               const ImRect &preview, bool list) {
        struct QuickAction {
            TextId label;
            Action action;
        };
        constexpr auto actions = std::to_array<QuickAction>({{TextId::HistoryQuickCopy, Action::Copy},
                                                             {TextId::HistoryQuickAnnotate, Action::Edit},
                                                             {TextId::HistoryFavorite, Action::Favorite},
                                                             {TextId::HistoryDelete, Action::Delete}});
        const auto &colors = GetHistoryPalette(m_theme);
        const float scale = m_uiScale;
        // 图片浮层使用整张卡片的宽度；列表视图的缩略图只占左侧，不能用其右边界限制操作栏。
        const auto position = bounds.Min;
        const float cardRight = bounds.Max.x;
        const ImVec2 barMaximum(cardRight - CardLayout::PreviewPadding * scale, preview.Max.y);
        const ImVec2 barMinimum(position.x + CardLayout::PreviewPadding * scale,
                                barMaximum.y - CardLayout::ActionBarHeight * scale);
        auto *draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(barMinimum, barMaximum, colors.actionOverlay,
                            list ? CardLayout::ListActionRounding * scale : 0, ImDrawFlags_RoundCornersBottom);
        std::array<ImVec2, actions.size()> textSizes;
        float textWidth = 0;
        for (size_t i = 0; i < actions.size(); ++i) {
            textSizes[i] = ImGui::CalcTextSize(Tr(m_language, actions[i].label));
            textWidth += textSizes[i].x;
        }
        const float padding = CardLayout::ActionTextPadding * scale,
                    separatorWidth = CardLayout::ActionSeparatorWidth * scale;
        const float totalPadding = 2 * actions.size() * padding, separators = (actions.size() - 1) * separatorWidth;
        // 只在翻译文字确实放不下时缩小字号，保持操作栏不溢出卡片。
        const float fontScale = std::min(
                1.f, (barMaximum.x - barMinimum.x - CardLayout::ActionSideReserve * scale - totalPadding - separators) /
                             textWidth);
        auto *font = ImGui::GetFont();
        const float fontSize = ImGui::GetFontSize() * fontScale;
        const float rowWidth = textWidth * fontScale + totalPadding + separators;
        float x = barMinimum.x + (barMaximum.x - barMinimum.x - rowWidth) * .5f;
        for (size_t i = 0; i < actions.size(); ++i) {
            const auto &action = actions[i];
            const auto textSize = textSizes[i] * fontScale;
            const float buttonWidth = textSize.x + 2 * padding;
            // 仍在原卡片的分组和 ID 作用域中，EndGroup 会恢复布局占用，避免卡片跳动。
            ImGui::SetCursorScreenPos(ImVec2(x, barMinimum.y));
            ImGui::PushID(int(i));
            if (ImGui::InvisibleButton("##history_quick_action", ImVec2(buttonWidth, barMaximum.y - barMinimum.y)))
                Queue(action.action, record.id, {}, {}, action.action != Action::Favorite || !record.details.favorite);
            if (action.action == Action::Delete && ImGui::IsItemHovered())
                draw->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                    GetThemePalette(m_theme).dangerHovered);
            ImU32 color =
                    action.action == Action::Delete ? ImGui::GetColorU32(GetThemePalette(m_theme).danger) : colors.text;
            if ((action.action == Action::Favorite && record.details.favorite) ||
                (action.action != Action::Delete && ImGui::IsItemHovered()))
                color = colors.link;
            draw->AddText(font, fontSize,
                          ImVec2(x + padding, barMinimum.y + (barMaximum.y - barMinimum.y - textSize.y) * .5f), color,
                          Tr(m_language, action.label));
            if (action.action == Action::Favorite) {
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, StyleLayout::TooltipPadding * scale);
                ImGui::SetItemTooltip("%s", Tr(m_language, record.details.favorite ? TextId::HistoryUnfavorite
                                                                                   : TextId::HistoryFavorite));
                ImGui::PopStyleVar();
            }
            ImGui::PopID();
            x += buttonWidth;
            if (i + 1 == actions.size())
                continue;
            const auto dotSize = font->CalcTextSizeA(fontSize, separatorWidth, 0, "·");
            draw->AddText(font, fontSize,
                          ImVec2(x + (separatorWidth - dotSize.x) * .5f,
                                 barMinimum.y + (barMaximum.y - barMinimum.y - dotSize.y) * .5f),
                          colors.muted, "·");
            x += separatorWidth;
        }
    }

    void HistoryWindow::RenderBrowser(float width, float height) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(BrowserLayout::Padding, BrowserLayout::Padding) * m_uiScale);
        ImGui::BeginChild("history_browser", ImVec2(width, height), ImGuiChildFlags_AlwaysUseWindowPadding);
        if (m_resetBrowserScroll) {
            ImGui::SetScrollY(0);
            m_resetBrowserScroll = false;
        }
        if (m_visible.empty())
            RenderEmptyHistory(width, height);
        else
            RenderHistoryCards(width);
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
    void HistoryWindow::RenderEmptyHistory(float width, float height) {
        const auto &colors = GetHistoryPalette(m_theme);
        const float scale = m_uiScale;
        const bool noRecords = std::none_of(m_records.begin(), m_records.end(),
                                            [](const auto &record) { return !record.details.deletedAtUnixMs; }) &&
                               m_query.scope != HistoryScope::Trash;
        const auto window = ImGui::GetWindowPos();
        const float center = window.x + width * .5f;
        const float top =
                window.y + std::max(BrowserLayout::EmptyMinTop * scale, height * BrowserLayout::EmptyTopFraction);
        const auto halfIcon = BrowserLayout::EmptyIconHalfSize * scale;
        ImGui::GetWindowDrawList()->AddRect(
                ImVec2(center - halfIcon.x, top), ImVec2(center + halfIcon.x, top + 2 * halfIcon.y), colors.border,
                StyleLayout::CornerRadius * scale, ImDrawFlags_None, BrowserLayout::EmptyIconStroke * scale);
        const auto title = Tr(m_language, noRecords ? TextId::HistoryNoScreenshots : TextId::HistoryNoMatches);
        ImGui::SetCursorScreenPos(
                ImVec2(center - ImGui::CalcTextSize(title).x * .5f, top + BrowserLayout::EmptyTitleTop * scale));
        ImGui::TextUnformatted(title);
        ImGui::SetCursorScreenPos(ImVec2(center - BrowserLayout::EmptyDescriptionWidth * scale * .5f,
                                         top + BrowserLayout::EmptyDescriptionTop * scale));
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + BrowserLayout::EmptyDescriptionWidth * scale);
        ImGui::TextDisabled(
                "%s", Tr(m_language, noRecords ? TextId::HistoryEmptyDescription : TextId::HistorySearchDescription));
        ImGui::PopTextWrapPos();
        if (noRecords)
            return;
        ImGui::SetCursorScreenPos(ImVec2(center - BrowserLayout::EmptyButtonSize.x * scale * .5f,
                                         top + BrowserLayout::EmptyButtonTop * scale));
        if (ImGuiUtils::PrimaryButton(Tr(m_language, TextId::HistoryClearFilters),
                                      BrowserLayout::EmptyButtonSize * scale, colors.accent)) {
            m_query.scope = HistoryScope::All;
            m_query.value.clear();
            m_search[0] = '\0';
            UpdateFilter(true);
        }
    }
    void HistoryWindow::RenderHistoryCards(float width) {
        const float scale = m_uiScale;
        const bool trash = m_query.scope == HistoryScope::Trash;
        if (trash) {
            ImGui::TextWrapped("%s", Tr(m_language, TextId::HistoryDeletedNotice));
            if (ImGuiUtils::DestructiveButton(Tr(m_language, TextId::HistoryEmptyTrash), GetThemePalette(m_theme)))
                Confirm(Action::EmptyTrash);
            ImGui::Separator();
        }
        const float available = ImGui::GetContentRegionAvail().x;
        const float gap = BrowserLayout::CardsGap * scale;
        // 列数只依赖稳定的窗口宽度，避免滚动条显隐造成反复换列。
        m_columns = m_listView ? 1
                               : std::max(1, int((width - 2 * BrowserLayout::Padding * scale + gap) /
                                                 (BrowserLayout::MinimumCardWidth * scale + gap)));
        const float cardWidth = m_listView ? available : (available - gap * (m_columns - 1)) / m_columns;
        const bool grouped = m_query.sort == HistorySort::Newest || m_query.sort == HistorySort::Oldest;
        const auto recordDay = [trash](const HistoryMetadata &record) {
            return HistoryDayStart(trash ? record.details.deletedAtUnixMs : record.createdAtUnixMs);
        };
        std::map<int64_t, size_t> dayCounts;
        if (grouped)
            for (const auto index: m_visible)
                ++dayCounts[recordDay(m_records[index])];
        int column = 0;
        int64_t day = -1;
        for (size_t position = 0; position < m_visible.size(); ++position) {
            const auto index = m_visible[position];
            const auto nextDay = recordDay(m_records[index]);
            if ((grouped && nextDay != day) || position == 0) {
                column = 0;
                day = nextDay;
                if (position)
                    ImGui::Dummy(ImVec2(0, BrowserLayout::DateGroupGap * scale));
                RenderDateGroupHeading(day, grouped ? dayCounts.at(day) : m_visible.size());
            }
            if (column)
                ImGui::SameLine(0, gap);
            RenderCard(int(index),
                       ImVec2(cardWidth,
                              (m_listView ? BrowserLayout::ListCardHeight : BrowserLayout::GridCardHeight) * scale),
                       m_listView);
            column = (column + 1) % m_columns;
        }
    }
    void HistoryWindow::RenderDateGroupHeading(int64_t day, size_t count) {
        std::string label;
        if (day == HistoryDayStart(HistoryNow()))
            label = Tr(m_language, TextId::HistoryToday);
        else if (day == HistoryDayStart(HistoryNow(), -1))
            label = Tr(m_language, TextId::HistoryYesterday);
        else
            label = HistoryTime(day, TextLayout::DateFormat);
        ImGui::TextUnformatted(label.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled(Tr(m_language, TextId::HistoryEntries), count);
    }

    void HistoryWindow::RenderDetails(float width, float height) {
        const float scale = m_uiScale;
        const auto &colors = GetHistoryPalette(m_theme);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, colors.sidebar);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                            ImVec2(DetailsLayout::Padding, DetailsLayout::Padding) * scale);
        ImGui::BeginChild("history_detail", ImVec2(width, height), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar);
        const auto position = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddLine(position, ImVec2(position.x, position.y + height), colors.border);
        const float available = width - 2 * DetailsLayout::Padding * scale;
        if (m_selected >= 0 && m_selected < int(m_records.size())) {
            const auto &record = m_records[m_selected];
            RenderDetailsInfo(record, available, height);
            RenderDetailsActions(record, available, height);
        } else if (!m_error.empty())
            ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
    }
    void HistoryWindow::RenderDetailsInfo(const HistoryMetadata &record, float width, float height) {
        ImGui::BeginChild("history_detail_info", ImVec2(width, height - DetailsLayout::ActionsReserve * m_uiScale));
        RenderDetailsPreview(width);
        RenderDetailsTitle(record);
        RenderDetailsMetadata(record, width);
        RenderDetailsTags(record);
        RenderOcrText(record, width);
        if (!m_editable)
            ImGui::TextWrapped("%s", Tr(m_language, TextId::HistoryReadOnly));
        if (!m_error.empty())
            ImGui::TextWrapped("%s", m_error.c_str());
        if (!m_notice.empty())
            ImGui::TextWrapped("%s", m_notice.c_str());
        ImGui::EndChild();
    }
    void HistoryWindow::RenderDetailsPreview(float width) {
        const auto &colors = GetHistoryPalette(m_theme);
        const float scale = m_uiScale;
        const auto position = ImGui::GetCursorScreenPos();
        const ImVec2 size(width, DetailsLayout::PreviewHeight * scale);
        ImGui::InvisibleButton("history_preview_open", size);
        if (ImGui::IsItemClicked() && m_previewTexture)
            ImGui::OpenPopup("history_preview_popup");
        auto *draw = ImGui::GetWindowDrawList();
        draw->AddRectFilled(position, position + size, colors.panel, StyleLayout::PanelCornerRadius * scale);
        draw->AddRect(position, position + size, colors.border, StyleLayout::PanelCornerRadius * scale);
        if (m_previewTexture && m_preview.width > 0 && m_preview.height > 0) {
            const float fit =
                    std::min((width - 2 * DetailsLayout::PreviewInset * scale) / m_preview.width,
                             (DetailsLayout::PreviewHeight * scale - 2 * DetailsLayout::PreviewInset * scale) /
                                     m_preview.height);
            const ImVec2 dimensions(m_preview.width * fit, m_preview.height * fit);
            const auto minimum = position + (size - dimensions) * .5f;
            draw->AddImage((ImTextureID) (intptr_t) m_previewTexture, minimum, minimum + dimensions);
        }
        RenderPreviewPopup();
    }
    void HistoryWindow::RenderPreviewPopup() {
        if (!ImGui::BeginPopup("history_preview_popup", ImGuiWindowFlags_NoSavedSettings))
            return;
        ImGui::SetNextItemWidth(DetailsLayout::ZoomSliderWidth * m_uiScale);
        ImGui::SliderFloat(Tr(m_language, TextId::HistoryZoom), &m_zoom, DetailsLayout::MinimumZoom,
                           DetailsLayout::MaximumZoom, "%.1fx");
        ImGui::BeginChild("history_zoom_preview", DetailsLayout::ZoomPreviewSize * m_uiScale, ImGuiChildFlags_None,
                          ImGuiWindowFlags_HorizontalScrollbar);
        if (m_previewTexture)
            ImGui::Image((ImTextureID) (intptr_t) m_previewTexture,
                         ImVec2(m_preview.width * m_zoom, m_preview.height * m_zoom));
        ImGui::EndChild();
        ImGui::EndPopup();
    }
    void HistoryWindow::RenderDetailsTitle(const HistoryMetadata &record) {
        ImGui::Dummy(ImVec2(0, DetailsLayout::TitleGap * m_uiScale));
        ImGui::SetNextItemWidth(DetailsLayout::FillRemainingWidth);
        if (m_focusTitle) {
            ImGui::SetKeyboardFocusHere();
            m_focusTitle = false;
        }
        const bool entered = ImGui::InputText("##history_title", m_titleInput, sizeof(m_titleInput),
                                              ImGuiInputTextFlags_EnterReturnsTrue);
        if ((entered || ImGui::IsItemDeactivatedAfterEdit()) && m_titleInput[0] && record.details.title != m_titleInput)
            Queue(Action::Rename, record.id, {}, m_titleInput);
        if (ImGui::IsItemHovered()) {
            // 避免继承详情面板较大的内边距，与收藏提示浮窗保持一致。
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, StyleLayout::TooltipPadding * m_uiScale);
            ImGui::SetTooltip("%s", Tr(m_language, TextId::HistoryRename));
            ImGui::PopStyleVar();
        }
    }
    void HistoryWindow::RenderDetailsField(TextId label, const std::string &value, float width) {
        ImGui::TextDisabled("%s", Tr(m_language, label));
        ImGui::SameLine(DetailsLayout::FieldLabelWidth * m_uiScale);
        ImGui::PushTextWrapPos(width);
        ImGui::TextUnformatted(value.c_str());
        ImGui::PopTextWrapPos();
    }
    void HistoryWindow::RenderDetailsMetadata(const HistoryMetadata &record, float width) {
        char dimensions[TextLayout::DescriptionCapacity];
        std::snprintf(dimensions, sizeof(dimensions), "%d×%d · %.0f KiB", record.width, record.height,
                      double(record.bytes) / TextLayout::BytesPerKibibyte);
        RenderDetailsField(TextId::HistorySize, dimensions, width);
        std::string source = record.details.sourceApplication;
        if (!record.details.sourceWindow.empty())
            source += (source.empty() ? "" : " · ") + record.details.sourceWindow;
        RenderDetailsField(TextId::HistorySource,
                           source.empty() ? Tr(m_language, TextId::HistoryUnknownSource) : source, width);
        RenderDetailsField(TextId::HistoryTime, HistoryTime(record.createdAtUnixMs, TextLayout::DateTimeFormat), width);
    }
    void HistoryWindow::RenderDetailsTags(const HistoryMetadata &record) {
        const float scale = m_uiScale;
        ImGui::TextDisabled("%s", Tr(m_language, TextId::HistoryTags));
        ImGui::SameLine(DetailsLayout::FieldLabelWidth * scale);
        ImGui::BeginGroup();
        const float width = ImGui::GetContentRegionAvail().x;
        const float right = ImGui::GetCursorScreenPos().x + width;
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float badgeWidth = record.details.deletedAtUnixMs ? 0 : TagLayout::BadgeOverflow * scale;
        for (size_t i = 0; i < record.details.tags.size(); ++i) {
            const auto &tag = record.details.tags[i];
            const float tagWidth =
                    std::min(width, ImGui::CalcTextSize(tag.c_str()).x + 2 * TagLayout::Padding.x * scale + badgeWidth);
            // 外侧删除角标也占用换行空间，避免相邻标签的文字与角标相撞。
            if (i > 0 && ImGui::GetItemRectMax().x + gap + tagWidth <= right)
                ImGui::SameLine(0, gap + badgeWidth);
            RenderTag(record, tag, width);
        }
        if (!record.details.deletedAtUnixMs) {
            if (m_tagEditor == TagEditor::Details)
                RenderTagInput(width);
            else if (ImGui::SmallButton(Tr(m_language, TextId::HistoryAddTag)))
                StartTagEdit({record.id}, TagEditor::Details);
        }
        ImGui::EndGroup();
    }
    void HistoryWindow::RenderOcrText(const HistoryMetadata &record, float width) {
        ImGui::Dummy(ImVec2(0, DetailsLayout::OcrTopGap * m_uiScale));
        ImGui::TextDisabled("%s", Tr(m_language, TextId::HistoryRecognizedText));
        if (!record.details.ocrIndexed || m_service.IsIndexing(record.id)) {
            ImGui::TextWrapped("%s", Tr(m_language, TextId::HistoryIndexing));
            return;
        }
        if (!record.details.ocrError.empty()) {
            ImGui::TextWrapped("%s", record.details.ocrError.c_str());
            if (ImGui::SmallButton(Tr(m_language, TextId::HistoryRetry)))
                Queue(Action::Index, record.id);
            return;
        }
        if (record.details.ocrText.empty()) {
            ImGui::TextDisabled("%s", Tr(m_language, TextId::HistoryOcrEmpty));
            return;
        }
        ImGui::BeginChild("history_ocr_text", ImVec2(width, DetailsLayout::OcrHeight * m_uiScale),
                          ImGuiChildFlags_Borders);
        ImGui::TextWrapped("%s", record.details.ocrText.c_str());
        ImGui::EndChild();
        if (ImGui::SmallButton(Tr(m_language, TextId::HistoryCopyText)))
            Queue(Action::CopyText, record.id);
    }
    void HistoryWindow::RenderDetailsActions(const HistoryMetadata &record, float width, float height) {
        const float scale = m_uiScale;
        const ImVec2 fullButton(width, DetailsLayout::ActionButtonHeight * scale);
        const ImVec2 halfButton((width - DetailsLayout::ActionGap * scale) * .5f, fullButton.y);
        ImGui::SetCursorPos(ImVec2(DetailsLayout::Padding * scale, height - DetailsLayout::ActionsFromBottom * scale));
        ImGui::BeginDisabled(m_busy);
        if (record.details.deletedAtUnixMs) {
            if (ImGuiUtils::PrimaryButton(Tr(m_language, TextId::HistoryRestore), fullButton,
                                          GetHistoryPalette(m_theme).accent))
                Queue(Action::Restore, record.id);
            if (ImGuiUtils::DestructiveButton(Tr(m_language, TextId::HistoryPurge), GetThemePalette(m_theme),
                                              fullButton))
                Confirm(Action::Purge, {record.id});
        } else {
            if (ImGuiUtils::PrimaryButton(Tr(m_language, TextId::HistoryCopy), halfButton,
                                          GetHistoryPalette(m_theme).accent))
                Queue(Action::Copy, record.id);
            ImGui::SameLine(0, DetailsLayout::ActionGap * scale);
            ImGui::BeginDisabled(!m_editable);
            if (ImGui::Button(Tr(m_language, TextId::HistoryEdit), halfButton))
                Queue(Action::Edit, record.id);
            ImGui::EndDisabled();
            if (ImGui::Button(Tr(m_language, TextId::HistoryExport), halfButton))
                Queue(Action::Save, record.id);
            ImGui::SameLine(0, DetailsLayout::ActionGap * scale);
            if (ImGuiUtils::DestructiveButton(Tr(m_language, TextId::HistoryDelete), GetThemePalette(m_theme),
                                              halfButton))
                Queue(Action::Delete, record.id);
        }
        ImGui::EndDisabled();
    }

    void HistoryWindow::RenderDialogs() {
        RenderStitchDialog();
        RenderConfirmDialog();
    }
    void HistoryWindow::RenderStitchDialog() {
        const auto title = std::string(Tr(m_language, TextId::HistoryStitch)) + "###history_stitch_dialog";
        if (m_openStitchDialog) {
            ImGui::OpenPopup(title.c_str());
            m_openStitchDialog = false;
        }
        ImGui::SetNextWindowSize(ImVec2(DialogLayout::Width * m_uiScale, 0), ImGuiCond_Appearing);
        if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        ImGui::TextUnformatted(Tr(m_language, TextId::HistoryStitch));
        if (ImGui::RadioButton(Tr(m_language, TextId::HistoryVertical), !m_stitchHorizontal))
            m_stitchHorizontal = false;
        ImGui::SameLine();
        if (ImGui::RadioButton(Tr(m_language, TextId::HistoryHorizontal), m_stitchHorizontal))
            m_stitchHorizontal = true;
        ImGui::PushTextWrapPos(DialogLayout::ContentWidth * m_uiScale);
        ImGui::TextWrapped("%s", Tr(m_language, TextId::HistoryStitchOrder));
        ImGui::PopTextWrapPos();
        RenderStitchOrder();
        if (ImGui::Button(Tr(m_language, TextId::HistoryStitchOpen))) {
            Queue(Action::Stitch, {}, m_stitchOrder, {}, m_stitchHorizontal);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(m_language, TextId::HistoryCancel)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    void HistoryWindow::RenderStitchOrder() {
        ImGui::BeginChild("history_stitch_order",
                          ImVec2(DialogLayout::ContentWidth, DialogLayout::OrderHeight) * m_uiScale);
        int swapFrom = -1, swapTo = -1;
        for (size_t i = 0; i < m_stitchOrder.size(); ++i) {
            ImGui::PushID(int(i));
            ImGui::BeginDisabled(i == 0);
            if (ImGui::ArrowButton("up", ImGuiDir_Up)) {
                swapFrom = int(i);
                swapTo = int(i) - 1;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(i + 1 == m_stitchOrder.size());
            if (ImGui::ArrowButton("down", ImGuiDir_Down)) {
                swapFrom = int(i);
                swapTo = int(i) + 1;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            const auto *record = FindRecord(m_stitchOrder[i]);
            ImGui::TextUnformatted(record ? HistoryName(*record, m_language).c_str() : m_stitchOrder[i].c_str());
            ImGui::PopID();
        }
        // 完成当前遍历后再调整顺序，保持本帧每一行的 ID 与目标记录一致。
        if (swapFrom >= 0)
            std::swap(m_stitchOrder[swapFrom], m_stitchOrder[swapTo]);
        ImGui::EndChild();
    }
    void HistoryWindow::RenderConfirmDialog() {
        const auto title = std::string(Tr(m_language, TextId::HistoryConfirm)) + "###history_confirm_dialog";
        if (m_openConfirmDialog) {
            ImGui::OpenPopup(title.c_str());
            m_openConfirmDialog = false;
        }
        if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize))
            return;
        const auto message =
                m_confirmAction == Action::EmptyTrash ? TextId::HistoryEmptyTrashConfirm : TextId::HistoryPurgeConfirm;
        ImGui::TextUnformatted(Tr(m_language, message));
        if (ImGuiUtils::DestructiveButton(Tr(m_language, TextId::HistoryConfirm), GetThemePalette(m_theme))) {
            Queue(m_confirmAction, {}, m_confirmTargets);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(Tr(m_language, TextId::HistoryCancel)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    void HistoryWindow::RenderToast() {
        if (m_toast.empty())
            return;
        if (std::chrono::steady_clock::now() >= m_toastUntil) {
            m_toast.clear();
            return;
        }
        const auto size = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2(size.x * .5f, size.y - ToastLayout::BottomPadding * m_uiScale), ImGuiCond_Always,
                                ImVec2(.5f, 1));
        ImGui::SetNextWindowBgAlpha(ToastLayout::BackgroundAlpha);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(GetHistoryPalette(m_theme).selected));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ToastLayout::Padding * m_uiScale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, StyleLayout::PanelCornerRadius * m_uiScale);
        // 提示浮层不参与卡片布局，也不抢占键盘焦点或拦截鼠标。
        ImGui::Begin("history_copy_toast", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                             ImGuiWindowFlags_NoInputs);
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
        ImGui::TextUnformatted(m_toast.c_str());
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }
    void HistoryWindow::RenderContent() {
        m_tagEditorVisible = false;
        const float scale = m_uiScale;
        const auto size = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(ImVec2());
        ImGui::SetNextWindowSize(size);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2());
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        // 点击内容会让 ImGui 把获得焦点的窗口提到渲染最上层，全屏主窗口会盖住
        // 右上角的共享窗口控制组；与编辑器视口一致禁止聚焦置顶。
        ImGui::Begin("history", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBringToFrontOnFocus);
        RenderHeader(size.x);
        const float top = HeaderLogicalHeight * scale;
        const float height = std::max(StyleLayout::MinimumViewportPixelExtent, size.y - top);
        ImGui::SetCursorPos(ImVec2(0, top));
        RenderSidebar(height);
        const float detailsWidth = DetailsLayout::Width * scale;
        const float browserWidth =
                std::max(StyleLayout::MinimumViewportPixelExtent, size.x - SidebarLayout::Width * scale - detailsWidth);
        ImGui::SetCursorPos(ImVec2((SidebarLayout::Width + BrowserLayout::Padding) * scale,
                                   top + BrowserLayout::SelectionTop * scale));
        const float selectionHeight = RenderSelectionHeader(
                std::max(StyleLayout::MinimumViewportPixelExtent, browserWidth - 2 * BrowserLayout::Padding * scale));
        const float browserTop = BrowserLayout::SelectionTop * scale + selectionHeight;
        ImGui::SetCursorPos(ImVec2(SidebarLayout::Width * scale, top + browserTop));
        RenderBrowser(browserWidth, std::max(StyleLayout::MinimumViewportPixelExtent, height - browserTop));
        ImGui::SetCursorPos(ImVec2(size.x - detailsWidth, top));
        RenderDetails(detailsWidth, height);
        // 筛选、菜单关闭或多选改变后，输入框未渲染也提交到编辑开始时固定的目标。
        if (m_tagEditor != TagEditor::None && !m_tagEditorVisible)
            FinishTagEdit();
        if (m_busy) {
            ImGui::SetCursorPos(
                    ImVec2(HeaderLayout::SearchPosition.x * scale, size.y - SelectionLayout::RowHeight * scale));
            ImGui::TextDisabled("%s", Tr(m_language, TextId::HistoryBusy));
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
#if defined(SHOTLET_PLATFORM_WINDOWS)
        // 无边框窗口在 header 右上角渲染共享窗口控制组，底色与 header 一致保持无缝。
        WindowChrome::RenderWindowControls("HistoryWindowControls", HeaderLogicalHeight * scale, m_theme, m_language,
                                           scale, (SDL_GetWindowFlags(m_window) & SDL_WINDOW_MAXIMIZED) != 0,
                                           /*includeMaximize=*/true, GetHistoryPalette(m_theme).sidebar,
                                           nullptr, [this](WindowChrome::WindowAction a) { HandleWindowAction(a); });
#endif
        RenderToast();
        RenderDialogs();
    }
} // namespace shotlet
