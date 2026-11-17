#include "SubToolbar.h"
#include "Toolbar.h"
#include "ToolbarIcons.h"
#include "core/Canvas.h"
#include "utils/ImGuiUtils.h"

#include <imgui.h>
#include <imgui_internal.h>

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <string>

namespace shotlet {
namespace SubToolbarLayout {
constexpr float Padding = 8;
constexpr float ItemSpacing = 12;
constexpr float ItemSize = 20;
constexpr float DividerSpacing = 16;
constexpr float ScreenMargin = 10;
constexpr float TransparencySliderWidth = 110;
constexpr float SelectionRounding = 2;
constexpr int HighlightShapeCount = 2;
// 水印输入框在提示文字宽度之外额外预留的编辑余量（逻辑像素）
constexpr float WatermarkInputSlack = 24;
constexpr float WatermarkInputPaddingX = 6;
}

void SubToolbar::SetCanvas(Canvas* canvas) {
    if (m_canvas == canvas) return;
    m_canvas = canvas;
    m_prevTool = ToolType::None;
    m_watermarkFocusPending = false;
    m_watermarkBuffer[0] = '\0';
}

// 渲染子工具栏入口，负责组织配置、布局和控件组绘制。
void SubToolbar::Render() {
    m_windowRect = ImRect(1, 1, 0, 0);  // 每帧重置，未渲染时保持无效

    if (!m_canvas || !m_toolbar) return;
    const ToolType tool = m_canvas->HasSelection() ? m_canvas->GetPropertyTool() : ToolType::None;
    // 进入水印工具时同步画布中的水印文字并请求输入框焦点。
    if (tool == ToolType::Watermark && m_prevTool != ToolType::Watermark) {
        m_watermarkFocusPending = true;
        std::snprintf(m_watermarkBuffer, sizeof(m_watermarkBuffer), "%s", m_canvas->GetWatermarkText().c_str());
    }
    // 关闭工具或失去选区的帧也要记录退出，下一次打开才能重新同步文字并聚焦。
    m_prevTool = tool;
    if (tool != ToolType::Watermark) m_watermarkFocusPending = false;
    if (!m_canvas->HasSelection()) return;
    Options options;
    if (!BuildOptions(options)) return;

    const Metrics metrics = BuildMetrics();
    Layout layout;
    if (!CalculateLayout(options, metrics, layout)) return;

    ApplyWindowLayout(layout, metrics);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(metrics.padding, metrics.padding));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0f * m_uiScale);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(GetThemePalette(m_theme).panelBackground));

    if (ImGui::Begin("SubToolbar", nullptr, flags)) {
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        RenderFrameAndArrow(drawList, layout, metrics);
        RenderControls(drawList, options, metrics);
    }
    ImGui::End();

    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
}

// 根据当前工具生成子工具栏需要显示的控件配置。
bool SubToolbar::BuildOptions(Options& options) const {
    options.tool = m_canvas->GetPropertyTool();
    options.hasSelectedLayer = m_canvas->HasSelectedLayer();
    options.showColors = true;

    switch (options.tool) {
        case ToolType::Rectangle:
        case ToolType::Ellipse:
            options.showFill = true;
            options.showThickness = true;
            return true;
        case ToolType::Arrow:
        case ToolType::Brush:
            options.showThickness = true;
            return true;
        case ToolType::Mosaic:
            options.showThickness = true;
            options.showColors = false;
            return true;
        case ToolType::Highlight:
            options.showHighlight = true;
            options.showColors = false;
            return true;
        case ToolType::Watermark:
            // 水印子工具栏保留标准颜色菜单，并提供文字与透明度编辑。
            options.showWatermarkInput = true;
            return true;
        case ToolType::Text:
            options.showFontSize = true;
            return true;
        case ToolType::Number:
            options.showNumberSize = true;
            return true;
        case ToolType::None:
            return false;
        default:
            return false;
    }
}

// 将 UI 缩放换算成子工具栏中复用的尺寸参数。
SubToolbar::Metrics SubToolbar::BuildMetrics() const {
    float s = m_uiScale;
    auto* captionFont = m_tooltipFont ? m_tooltipFont : ImGui::GetFont();
    const float captionSize = m_tooltipFont ? m_tooltipFont->LegacySize : ImMin(ImGui::GetFontSize(), SubToolbarLayout::ItemSize * m_uiScale);
    const auto transparencyLabel = m_canvas->GetPropertyTool() == ToolType::Watermark
        ? TextId::WatermarkOpacity : TextId::HighlightTransparency;
    const float transparencyLabelWidth = captionFont->CalcTextSizeA(captionSize, FLT_MAX, 0,
        Tr(m_language, transparencyLabel)).x / m_uiScale;
    const float transparencyValueWidth = captionFont->CalcTextSizeA(captionSize, FLT_MAX, 0, "100").x / m_uiScale;
    const float transparencyControlsWidth = transparencyLabelWidth + SubToolbarLayout::ItemSpacing +
        SubToolbarLayout::TransparencySliderWidth + SubToolbarLayout::ItemSpacing / 2 + transparencyValueWidth;
    // 水印输入框宽度按当前语言的提示文字计算，保证 placeholder 完整可见。
    const float watermarkHintWidth = captionFont->CalcTextSizeA(captionSize, FLT_MAX, 0,
        Tr(m_language, TextId::WatermarkHint)).x / m_uiScale;
    if (m_canvas->GetPropertyTool() == ToolType::Highlight) {
        const float baseWidth = SubToolbarLayout::HighlightShapeCount * SubToolbarLayout::ItemSize + SubToolbarLayout::ItemSpacing +
            SubToolbarLayout::DividerSpacing + transparencyControlsWidth + 2 * SubToolbarLayout::Padding;
        const float available = ImMax(1.0f, ImGui::GetIO().DisplaySize.x - 2 * SubToolbarLayout::ScreenMargin * m_uiScale);
        s = ImMin(s, available / baseWidth);
    }
    if (m_canvas->GetPropertyTool() == ToolType::Number) {
        const int controlCount = Canvas::NUMBER_SIZE_COUNT + Canvas::PRESET_COLOR_COUNT;
        const float baseWidth = controlCount * SubToolbarLayout::ItemSize + (controlCount - 2) * SubToolbarLayout::ItemSpacing +
            SubToolbarLayout::DividerSpacing + 2 * SubToolbarLayout::Padding;
        // 窄窗口下整组控件等比例缩小，保留全部尺寸与颜色选项。
        const float available = ImMax(1.0f, ImGui::GetIO().DisplaySize.x - 2 * SubToolbarLayout::ScreenMargin * m_uiScale);
        s = ImMin(s, available / baseWidth);
    }

    if (m_canvas->GetPropertyTool() == ToolType::Watermark) {
        const float baseWidth = Canvas::PRESET_COLOR_COUNT * SubToolbarLayout::ItemSize +
            (Canvas::PRESET_COLOR_COUNT - 1) * SubToolbarLayout::ItemSpacing +
            SubToolbarLayout::DividerSpacing + watermarkHintWidth + SubToolbarLayout::WatermarkInputSlack +
            SubToolbarLayout::DividerSpacing + transparencyControlsWidth +
            2 * SubToolbarLayout::Padding;
        // 窄窗口下整组控件等比例缩小，保留颜色、输入框和透明度控件。
        const float available = ImMax(1.0f, ImGui::GetIO().DisplaySize.x - 2 * SubToolbarLayout::ScreenMargin * m_uiScale);
        s = ImMin(s, available / baseWidth);
    }

    Metrics metrics;
    metrics.scale = s;
    metrics.transparencyLabelWidth = transparencyLabelWidth * s;
    metrics.transparencySliderWidth = SubToolbarLayout::TransparencySliderWidth * s;
    metrics.transparencyValueWidth = transparencyValueWidth * s;
    metrics.watermarkInputWidth = (watermarkHintWidth + SubToolbarLayout::WatermarkInputSlack) * s;
    metrics.padding = SubToolbarLayout::Padding * s;
    metrics.itemSpacing = SubToolbarLayout::ItemSpacing * s;
    metrics.dotRadiusSmall = 2.0f * s;
    metrics.dotRadiusMedium = 4.0f * s;
    metrics.dotRadiusLarge = 6.0f * s;
    metrics.fillIconSize = 18.0f * s;
    metrics.fillItemSize = SubToolbarLayout::ItemSize * s;
    metrics.fontSizeItemSize = SubToolbarLayout::ItemSize * s;
    metrics.colorSquareSize = 16.0f * s;
    metrics.colorItemSize = SubToolbarLayout::ItemSize * s;
    metrics.dividerSpacing = SubToolbarLayout::DividerSpacing * s;
    metrics.arrowHeight = 12.0f * s;
    metrics.arrowWidth = 18.0f * s;
    metrics.height = SubToolbarLayout::ItemSize * s + 2 * metrics.padding;
    return metrics;
}

// 计算子工具栏相对主工具栏选中图标的窗口位置和朝向。
bool SubToolbar::CalculateLayout(const Options& options, const Metrics& metrics, Layout& layout) const {
    ImRect selection = m_canvas->GetSelection();
    ImRect toolbarRect = m_toolbar->GetWindowRect();
    bool hasToolbarRect = toolbarRect.Min.x <= toolbarRect.Max.x && toolbarRect.Min.y <= toolbarRect.Max.y;
    bool toolbarAboveSelection = hasToolbarRect && toolbarRect.GetCenter().y < selection.GetCenter().y;

    if (!m_toolbar->HasSelectedToolPos()) return false;

    ImVec2 anchorPos = m_toolbar->GetSelectedToolPos();
    float anchorSize = m_toolbar->GetSelectedIconSize();
    float anchorTopY = anchorPos.y;
    float anchorBottomY = anchorPos.y + anchorSize;

    layout.selectedCenterX = anchorPos.x + anchorSize / 2.0f;
    layout.placeAboveAnchor = m_toolbar->IsEditorMode() ? false : toolbarAboveSelection;
    layout.height = metrics.height;

    float thicknessFillWidth = 0.0f;
    if (options.showFill) {
        // 矩形/椭圆：3 个粗细点和 1 个填充图标组成互斥选项组。
        thicknessFillWidth = Canvas::THICKNESS_COUNT * (metrics.colorItemSize + metrics.itemSpacing) - metrics.itemSpacing
            + metrics.itemSpacing + metrics.fillItemSize;
    } else if (options.showThickness) {
        thicknessFillWidth = Canvas::THICKNESS_COUNT * (metrics.colorItemSize + metrics.itemSpacing) - metrics.itemSpacing;
    }

    const int sizeCount = options.showNumberSize ? Canvas::NUMBER_SIZE_COUNT : Canvas::TEXT_FONT_SIZE_COUNT;
    float fontSizeWidth = (options.showFontSize || options.showNumberSize)
        ? sizeCount * (metrics.fontSizeItemSize + metrics.itemSpacing) - metrics.itemSpacing
        : 0.0f;
    float dividerWidth = (options.showColors && (options.showThickness || options.showFill || options.showFontSize || options.showNumberSize))
        ? metrics.dividerSpacing
        : 0.0f;
    float colorsWidth = options.showColors
        ? Canvas::PRESET_COLOR_COUNT * (metrics.colorItemSize + metrics.itemSpacing) - metrics.itemSpacing
        : 0.0f;

    layout.width = thicknessFillWidth + fontSizeWidth + dividerWidth + colorsWidth + 2 * metrics.padding;
    if (options.showHighlight) {
        layout.width = SubToolbarLayout::HighlightShapeCount * metrics.fillItemSize + metrics.itemSpacing + metrics.dividerSpacing +
            metrics.transparencyLabelWidth + metrics.itemSpacing + metrics.transparencySliderWidth + metrics.itemSpacing / 2 +
            metrics.transparencyValueWidth + 2 * metrics.padding;
    }
    if (options.showWatermarkInput) {
        layout.width = colorsWidth + metrics.dividerSpacing + metrics.watermarkInputWidth + metrics.dividerSpacing +
            metrics.transparencyLabelWidth + metrics.itemSpacing + metrics.transparencySliderWidth + metrics.itemSpacing / 2 +
            metrics.transparencyValueWidth + 2 * metrics.padding;
    }
    layout.x = layout.selectedCenterX - layout.width / 2.0f;

    ImGuiIO& io = ImGui::GetIO();
    const float s = m_uiScale;
    const float anchorGap = 16.0f * s;
    layout.y = layout.placeAboveAnchor
        ? anchorTopY - anchorGap - metrics.arrowHeight - layout.height
        : anchorBottomY + anchorGap + metrics.arrowHeight;

    if (!layout.placeAboveAnchor && layout.y + layout.height > io.DisplaySize.y - 10.0f * s) {
        layout.placeAboveAnchor = true;
        layout.y = anchorTopY - anchorGap - metrics.arrowHeight - layout.height;
    }
    if (layout.placeAboveAnchor && layout.y - metrics.arrowHeight < 10.0f * s) {
        layout.placeAboveAnchor = false;
        layout.y = anchorBottomY + anchorGap + metrics.arrowHeight;
    }

    if (layout.x + layout.width > io.DisplaySize.x - 10.0f * s) {
        layout.x = io.DisplaySize.x - 10.0f * s - layout.width;
    }
    if (layout.x < 10.0f * s) {
        layout.x = 10.0f * s;
    }

    return true;
}

// 将计算后的布局应用到 ImGui 窗口，并更新点击命中的窗口矩形。
void SubToolbar::ApplyWindowLayout(const Layout& layout, const Metrics& metrics) {
    ImGui::SetNextWindowPos(ImVec2(layout.x, layout.y));
    ImGui::SetNextWindowSize(ImVec2(layout.width, layout.height));

    m_windowRect = layout.placeAboveAnchor
        ? ImRect(layout.x, layout.y,
                 layout.x + layout.width, layout.y + layout.height + metrics.arrowHeight)
        : ImRect(layout.x, layout.y - metrics.arrowHeight,
                 layout.x + layout.width, layout.y + layout.height);
}

// 绘制子工具栏背景边框和指向主工具栏图标的三角形箭头。
void SubToolbar::RenderFrameAndArrow(ImDrawList* drawList, const Layout& layout, const Metrics& metrics) const {
    drawList->PushClipRectFullScreen();
    drawList->AddRect(
        ImVec2(layout.x, layout.y),
        ImVec2(layout.x + layout.width, layout.y + layout.height),
        GetThemePalette(m_theme).border, 2.0f, 0, 0.01f);

    float triLeft = layout.selectedCenterX - metrics.arrowWidth / 2.0f;
    float triRight = layout.selectedCenterX + metrics.arrowWidth / 2.0f;
    if (layout.placeAboveAnchor) {
        float triBase = layout.y + layout.height - 1.0f;
        float triTip = layout.y + layout.height + metrics.arrowHeight;
        drawList->AddTriangleFilled(
            ImVec2(triLeft, triBase),
            ImVec2(triRight, triBase),
            ImVec2(layout.selectedCenterX, triTip),
            GetThemePalette(m_theme).panelBackground);
        drawList->AddLine(
            ImVec2(layout.selectedCenterX, triTip),
            ImVec2(triLeft, triBase),
            GetThemePalette(m_theme).border, 0.1f);
        drawList->AddLine(
            ImVec2(layout.selectedCenterX, triTip),
            ImVec2(triRight, triBase),
            GetThemePalette(m_theme).border, 0.1f);
    } else {
        float triTop = layout.y - metrics.arrowHeight;
        float triBottom = layout.y + 1.0f;
        drawList->AddTriangleFilled(
            ImVec2(triLeft, triBottom),
            ImVec2(triRight, triBottom),
            ImVec2(layout.selectedCenterX, triTop),
            GetThemePalette(m_theme).panelBackground);
        drawList->AddLine(
            ImVec2(layout.selectedCenterX, triTop),
            ImVec2(triLeft, triBottom),
            GetThemePalette(m_theme).border, 0.1f);
        drawList->AddLine(
            ImVec2(layout.selectedCenterX, triTop),
            ImVec2(triRight, triBottom),
            GetThemePalette(m_theme).border, 0.1f);
    }

    drawList->PopClipRect();
}

// 渲染当前工具对应的全部子控件组。
void SubToolbar::RenderControls(ImDrawList* drawList, const Options& options, const Metrics& metrics) {
    if (options.showHighlight) { RenderHighlightControls(drawList, metrics); return; }
    if (options.showWatermarkInput) {
        // 沿用标准色块与输入框，右侧追加和高亮同款的透明度滑条。
        RenderColorControls(drawList, metrics);
        ImGui::SameLine(0, metrics.dividerSpacing / 2.0f);
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine(0, metrics.dividerSpacing / 2.0f);
        RenderWatermarkInput(metrics);
        ImGui::SameLine(0, metrics.dividerSpacing / 2);
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine(0, metrics.dividerSpacing / 2);
        int percent = m_canvas->GetWatermarkOpacity();
        if (RenderTransparencyControl("##watermark_opacity", TextId::WatermarkOpacity, percent,
            WatermarkSettings::MinimumOpacity, WatermarkSettings::MaximumOpacity, metrics)) m_canvas->SetWatermarkOpacity(percent);
        return;
    }
    if (options.showFill) {
        RenderStrokeFillControls(drawList, options, metrics);
    }

    if (options.showThickness && !options.showFill) {
        if (options.tool == ToolType::Mosaic) {
            RenderMosaicBrushControls(drawList, metrics);
        } else {
            RenderThicknessControls(drawList, options, metrics);
        }
    }

    if (options.showFontSize) {
        RenderFontSizeControls(drawList, metrics);
    }
    if (options.showNumberSize) RenderNumberSizeControls(drawList, metrics);

    RenderDivider(options, metrics);

    if (options.showColors) {
        RenderColorControls(drawList, metrics);
    }
}

void SubToolbar::RenderHighlightControls(ImDrawList* drawList, const Metrics& metrics) {
    const auto& palette = GetThemePalette(m_theme);
    for (const bool ellipse : {false, true}) {
        const auto position = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(ellipse ? "highlight_ellipse" : "highlight_rectangle", ImVec2(metrics.fillItemSize, metrics.fillItemSize));
        const bool selected = m_canvas->GetHighlightEllipse() == ellipse;
        if (selected || ImGui::IsItemHovered()) {
            drawList->AddRectFilled(position, ImVec2(position.x + metrics.fillItemSize, position.y + metrics.fillItemSize),
                palette.hoverBackground, SubToolbarLayout::SelectionRounding * metrics.scale);
        }
        const float inset = (metrics.fillItemSize - metrics.fillIconSize) / 2;
        ToolbarIcons::DrawIcon(drawList, ImVec2(position.x + inset, position.y + inset), metrics.fillIconSize,
            ellipse ? ToolbarIcons::IconType::Circle : ToolbarIcons::IconType::Rectangle,
            selected ? palette.accent : palette.icon, metrics.scale);
        if (ImGui::IsItemHovered()) ShowTooltip(Tr(m_language, ellipse ? TextId::ToolEllipse : TextId::ToolRectangle));
        if (ImGui::IsItemClicked()) m_canvas->SetHighlightEllipse(ellipse);
        if (!ellipse) ImGui::SameLine(0, metrics.itemSpacing);
    }
    ImGui::SameLine(0, metrics.dividerSpacing / 2);
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine(0, metrics.dividerSpacing / 2);
    int percent = m_canvas->GetHighlightTransparency();
    if (RenderTransparencyControl("##highlight_transparency", TextId::HighlightTransparency, percent,
        HighlightSettings::MinimumTransparency, HighlightSettings::MaximumTransparency, metrics)) m_canvas->SetHighlightTransparency(percent);
}

bool SubToolbar::RenderTransparencyControl(const char* id, TextId label, int& percent, int minimum, int maximum, const Metrics& metrics) {
    const auto& palette = GetThemePalette(m_theme);
    auto* font = m_tooltipFont ? m_tooltipFont : ImGui::GetFont();
    const float baseFontSize = m_tooltipFont ? m_tooltipFont->LegacySize : ImMin(ImGui::GetFontSize(), SubToolbarLayout::ItemSize * m_uiScale);
    const float fontSize = baseFontSize * metrics.scale / m_uiScale;
    ImGui::PushFont(font, fontSize);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette.text));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, ImMax(0.0f, (metrics.fillItemSize - fontSize) / 2)));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Tr(m_language, label));
    ImGui::SameLine(0, metrics.itemSpacing);
    ImGui::SetNextItemWidth(metrics.transparencySliderWidth);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(palette.hoverBackground));
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImGui::ColorConvertU32ToFloat4(palette.icon));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImGui::ColorConvertU32ToFloat4(palette.accent));
    const bool changed = ImGuiUtils::SliderIntWithCircularGrab(id, &percent, minimum, maximum);
    ImGui::SameLine(0, metrics.itemSpacing / 2);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%d", percent);
    ImGui::PopStyleColor(3);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::PopFont();
    return changed;
}

// 渲染水印文字输入框：默认聚焦，在编辑回调中限制字符数，再将结果存入画布。
void SubToolbar::RenderWatermarkInput(const Metrics& metrics) {
    const auto& palette = GetThemePalette(m_theme);
    auto* font = m_tooltipFont ? m_tooltipFont : ImGui::GetFont();
    const float baseFontSize = m_tooltipFont ? m_tooltipFont->LegacySize : ImMin(ImGui::GetFontSize(), SubToolbarLayout::ItemSize * m_uiScale);
    const float fontSize = baseFontSize * metrics.scale / m_uiScale;
    ImGui::PushFont(font, fontSize);
    // 垂直内边距让单行输入框与色块行同高，文字在行内居中。
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
        ImVec2(SubToolbarLayout::WatermarkInputPaddingX * metrics.scale, ImMax(0.0f, (metrics.fillItemSize - fontSize) / 2)));
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4(palette.hoverBackground));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(palette.text));
    ImGui::PushStyleColor(ImGuiCol_NavCursor, palette.accent);
    ImGui::SetNextItemWidth(metrics.watermarkInputWidth);
    if (m_watermarkFocusPending) {
        // 裁剪会更换 Canvas，但控件 ID 不变；先结束旧编辑会话，防止内部缓冲覆盖新文字。
        ImGuiUtils::ResetTextInputSession(ImGui::GetID("##watermark_input"));
        ImGui::SetKeyboardFocusHere();
        m_watermarkFocusPending = false;
    }
    if (ImGuiUtils::InputTextWithCharacterLimit("##watermark_input", Tr(m_language, TextId::WatermarkHint),
        m_watermarkBuffer, sizeof(m_watermarkBuffer), WatermarkSettings::MaxCharacters)) {
        m_canvas->SetWatermarkText(m_watermarkBuffer);
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();
    ImGui::PopFont();
}

// 渲染矩形/椭圆工具的描边粗细与填充互斥选择。
void SubToolbar::RenderStrokeFillControls(ImDrawList* drawList, const Options& options, const Metrics& metrics) {
    bool isFilled = options.hasSelectedLayer && m_canvas->HasSelectedFillableLayer()
        ? m_canvas->GetSelectedLayerFilled()
        : m_canvas->GetToolFill();
    float currentThickness = options.hasSelectedLayer && m_canvas->HasSelectedFillableLayer()
        ? m_canvas->GetSelectedLayerThickness()
        : m_canvas->GetToolThickness();

    float dotRadii[] = {metrics.dotRadiusSmall, metrics.dotRadiusMedium, metrics.dotRadiusLarge};

    for (int i = 0; i < Canvas::THICKNESS_COUNT; i++) {
        ImVec2 p_min = ImGui::GetCursorScreenPos();
        ImVec2 center(p_min.x + metrics.colorItemSize / 2.0f, p_min.y + metrics.colorItemSize / 2.0f);
        float r = dotRadii[i];
        bool isSelected = !isFilled && (currentThickness == Canvas::THICKNESS_OPTIONS[i]);

        ImGui::InvisibleButton(("thickness_" + std::to_string(i)).c_str(), ImVec2(metrics.colorItemSize, metrics.colorItemSize));

        if (!isSelected && ImGui::IsItemHovered()) {
            drawList->AddRectFilled(
                p_min,
                ImVec2(p_min.x + metrics.colorItemSize, p_min.y + metrics.colorItemSize),
                GetThemePalette(m_theme).hoverBackground,
                2.0f * m_uiScale);
        }

        drawList->AddCircleFilled(center, r, isSelected ? GetThemePalette(m_theme).accent : GetThemePalette(m_theme).inactiveOption);

        if (ImGui::IsItemHovered()) {
            const char* thicknessLabels[] = {Tr(m_language, TextId::StrokeThin), Tr(m_language, TextId::StrokeMedium), Tr(m_language, TextId::StrokeThick)};
            ShowTooltip(thicknessLabels[i]);
        }

        if (ImGui::IsItemClicked()) {
            if (options.hasSelectedLayer && m_canvas->HasSelectedFillableLayer()) {
                m_canvas->SetSelectedLayerFilled(false);
                m_canvas->SetSelectedLayerThickness(Canvas::THICKNESS_OPTIONS[i]);
            } else {
                m_canvas->SetToolFill(false);
                m_canvas->SetToolThickness(Canvas::THICKNESS_OPTIONS[i]);
            }
        }

        if (i < Canvas::THICKNESS_COUNT - 1) {
            ImGui::SameLine(0, metrics.itemSpacing);
        }
    }

    ImGui::SameLine(0, metrics.itemSpacing);
    ImVec2 p_min = ImGui::GetCursorScreenPos();
    ImVec2 rectMin(p_min.x + (metrics.fillItemSize - metrics.fillIconSize) / 2.0f,
                   p_min.y + (metrics.fillItemSize - metrics.fillIconSize) / 2.0f);
    ImVec2 rectMax(rectMin.x + metrics.fillIconSize, rectMin.y + metrics.fillIconSize);

    ImGui::InvisibleButton("fill_btn", ImVec2(metrics.fillItemSize, metrics.fillItemSize));

    if (!isFilled && ImGui::IsItemHovered()) {
        drawList->AddRectFilled(
            p_min,
            ImVec2(p_min.x + metrics.fillItemSize, p_min.y + metrics.fillItemSize),
            GetThemePalette(m_theme).hoverBackground,
            2.0f * m_uiScale);
    }

    drawList->AddRectFilled(rectMin, rectMax, isFilled ? GetThemePalette(m_theme).accent : GetThemePalette(m_theme).fillIcon, 1.0f * m_uiScale);

    rectMin.x += 1.5f;
    rectMin.y += 1.5f;
    rectMax.x -= 1.5f;
    rectMax.y -= 1.5f;
    drawList->AddRect(rectMin, rectMax, GetThemePalette(m_theme).panelBackground, 0.0f, 0, 1.5f * m_uiScale);

    if (ImGui::IsItemHovered()) {
        ShowTooltip(Tr(m_language, TextId::Fill));
    }

    if (ImGui::IsItemClicked()) {
        if (options.hasSelectedLayer && m_canvas->HasSelectedFillableLayer()) {
            m_canvas->SetSelectedLayerFilled(true);
        } else {
            m_canvas->SetToolFill(true);
        }
    }
}

// 渲染箭头和画笔工具的描边粗细选择。
void SubToolbar::RenderThicknessControls(ImDrawList* drawList, const Options& options, const Metrics& metrics) {
    float currentThickness = options.hasSelectedLayer && !m_canvas->HasSelectedTextLayer()
        ? m_canvas->GetSelectedLayerThickness()
        : m_canvas->GetToolThickness();

    float dotRadii[] = {metrics.dotRadiusSmall, metrics.dotRadiusMedium, metrics.dotRadiusLarge};
    for (int i = 0; i < Canvas::THICKNESS_COUNT; i++) {
        ImVec2 p_min = ImGui::GetCursorScreenPos();
        ImVec2 center(p_min.x + metrics.colorItemSize / 2.0f, p_min.y + metrics.colorItemSize / 2.0f);
        float r = dotRadii[i];
        bool isSelected = (currentThickness == Canvas::THICKNESS_OPTIONS[i]);

        ImGui::InvisibleButton(("thickness_" + std::to_string(i)).c_str(), ImVec2(metrics.colorItemSize, metrics.colorItemSize));

        if (!isSelected && ImGui::IsItemHovered()) {
            drawList->AddRectFilled(
                p_min,
                ImVec2(p_min.x + metrics.colorItemSize, p_min.y + metrics.colorItemSize),
                GetThemePalette(m_theme).hoverBackground,
                2.0f * m_uiScale);
        }

        drawList->AddCircleFilled(center, r, isSelected ? GetThemePalette(m_theme).accent : GetThemePalette(m_theme).inactiveOption);

        if (ImGui::IsItemHovered()) {
            const char* thicknessLabels[] = {Tr(m_language, TextId::StrokeThin), Tr(m_language, TextId::StrokeMedium), Tr(m_language, TextId::StrokeThick)};
            ShowTooltip(thicknessLabels[i]);
        }

        if (ImGui::IsItemClicked()) {
            if (options.hasSelectedLayer && !m_canvas->HasSelectedTextLayer()) {
                m_canvas->SetSelectedLayerThickness(Canvas::THICKNESS_OPTIONS[i]);
            } else {
                m_canvas->SetToolThickness(Canvas::THICKNESS_OPTIONS[i]);
            }
        }

        if (i < Canvas::THICKNESS_COUNT - 1) {
            ImGui::SameLine(0, metrics.itemSpacing);
        }
    }
}

// 渲染马赛克工具的笔刷大小选择。
void SubToolbar::RenderMosaicBrushControls(ImDrawList* drawList, const Metrics& metrics) {
    int currentBrushIndex = m_canvas->GetMosaicBrushIndex();
    float dotRadii[] = {metrics.dotRadiusSmall, metrics.dotRadiusMedium, metrics.dotRadiusLarge};

    for (int i = 0; i < Canvas::MOSAIC_BRUSH_COUNT; i++) {
        ImVec2 p_min = ImGui::GetCursorScreenPos();
        ImVec2 center(p_min.x + metrics.colorItemSize / 2.0f, p_min.y + metrics.colorItemSize / 2.0f);
        float r = dotRadii[i];
        bool isSelected = (currentBrushIndex == i);

        ImGui::InvisibleButton(("mosaic_brush_" + std::to_string(i)).c_str(), ImVec2(metrics.colorItemSize, metrics.colorItemSize));

        if (!isSelected && ImGui::IsItemHovered()) {
            drawList->AddRectFilled(
                p_min,
                ImVec2(p_min.x + metrics.colorItemSize, p_min.y + metrics.colorItemSize),
                GetThemePalette(m_theme).hoverBackground,
                2.0f * m_uiScale);
        }

        drawList->AddCircleFilled(center, r, isSelected ? GetThemePalette(m_theme).accent : GetThemePalette(m_theme).inactiveOption);

        if (ImGui::IsItemHovered()) {
            const char* brushLabels[] = {Tr(m_language, TextId::BrushSmall), Tr(m_language, TextId::BrushMedium), Tr(m_language, TextId::BrushLarge)};
            ShowTooltip(brushLabels[i]);
        }

        if (ImGui::IsItemClicked()) {
            m_canvas->SetMosaicBrushIndex(i);
        }

        if (i < Canvas::MOSAIC_BRUSH_COUNT - 1) {
            ImGui::SameLine(0, metrics.itemSpacing);
        }
    }
}

// 渲染文字工具的字号选择。
void SubToolbar::RenderFontSizeControls(ImDrawList* drawList, const Metrics& metrics) {
    const char* fontSizeLabels[] = {Tr(m_language, TextId::SizeSmallLabel), Tr(m_language, TextId::SizeMediumLabel), Tr(m_language, TextId::SizeLargeLabel)};
    const TextId fontSizeTips[] = {TextId::SizeSmall, TextId::SizeMedium, TextId::SizeLarge};
    int currentFontSizeIndex;
    if (m_canvas->HasSelectedTextLayer()) {
        float layerFontSize = m_canvas->GetSelectedTextLayerFontSize();
        currentFontSizeIndex = 1;
        float minDiff = FLT_MAX;
        for (int j = 0; j < Canvas::TEXT_FONT_SIZE_COUNT; j++) {
            float diff = std::fabs(layerFontSize - m_canvas->GetScaledTextFontSizeForIndex(j));
            if (diff < minDiff) {
                minDiff = diff;
                currentFontSizeIndex = j;
            }
        }
    } else {
        currentFontSizeIndex = m_canvas->GetTextFontSizeIndex();
    }

    for (int i = 0; i < Canvas::TEXT_FONT_SIZE_COUNT; i++) {
        ImVec2 p_min = ImGui::GetCursorScreenPos();
        bool isSelected = (currentFontSizeIndex == i);

        ImGui::InvisibleButton(("fontsize_" + std::to_string(i)).c_str(),
                                ImVec2(metrics.fontSizeItemSize, metrics.fontSizeItemSize));

        if (!isSelected && ImGui::IsItemHovered()) {
            drawList->AddRectFilled(
                p_min,
                ImVec2(p_min.x + metrics.fontSizeItemSize, p_min.y + metrics.fontSizeItemSize),
                GetThemePalette(m_theme).hoverBackground,
                2.0f * m_uiScale);
        }

        if (m_tooltipFont) {
            float renderSize = m_tooltipFont->LegacySize;
            ImVec2 textSize = m_tooltipFont->CalcTextSizeA(renderSize, FLT_MAX, 0.0f, fontSizeLabels[i]);
            ImVec2 textPos(
                p_min.x + (metrics.fontSizeItemSize - textSize.x) / 2.0f,
                p_min.y + (metrics.fontSizeItemSize - textSize.y) / 2.0f
            );
            ImU32 textColor = isSelected ? GetThemePalette(m_theme).accent : GetThemePalette(m_theme).icon;
            drawList->AddText(m_tooltipFont, renderSize, textPos, textColor, fontSizeLabels[i]);
        }

        if (ImGui::IsItemHovered()) {
            ShowTooltip(Tr(m_language, fontSizeTips[i]));
        }

        if (ImGui::IsItemClicked()) {
            m_canvas->SetTextFontSizeIndex(i);
        }

        if (i < Canvas::TEXT_FONT_SIZE_COUNT - 1) {
            ImGui::SameLine(0, metrics.itemSpacing);
        }
    }
}

// 在属性控件与颜色控件之间按需渲染分割线。
void SubToolbar::RenderDivider(const Options& options, const Metrics& metrics) const {
    bool hasOtherOptions = options.showThickness || options.showFill || options.showFontSize || options.showNumberSize;
    if (hasOtherOptions && options.showColors) {
        ImGui::SameLine(0, metrics.dividerSpacing / 2.0f);
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine(0, metrics.dividerSpacing / 2.0f);
    }
}

void SubToolbar::RenderNumberSizeControls(ImDrawList* drawList, const Metrics& metrics) {
    const int selected = m_canvas->GetNumberSizeIndex();
    const char* labels[] = {Tr(m_language, TextId::SizeSmall), Tr(m_language, TextId::SizeMedium), Tr(m_language, TextId::SizeLarge)};
    for (int i = 0; i < Canvas::NUMBER_SIZE_COUNT; ++i) {
        if (i > 0) ImGui::SameLine(0, metrics.itemSpacing);
        const ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton(("number_size_" + std::to_string(i)).c_str(),
                               ImVec2(metrics.fontSizeItemSize, metrics.fontSizeItemSize));
        if (selected == i || ImGui::IsItemHovered()) {
            drawList->AddRectFilled(pos, ImVec2(pos.x + metrics.fontSizeItemSize, pos.y + metrics.fontSizeItemSize),
                                    GetThemePalette(m_theme).hoverBackground, m_uiScale);
        }
        drawList->AddCircleFilled(ImVec2(pos.x + metrics.fontSizeItemSize * 0.5f, pos.y + metrics.fontSizeItemSize * 0.5f),
                                  (3.0f + i * 2.0f) * metrics.fontSizeItemSize / 20.0f,
                                  selected == i ? GetThemePalette(m_theme).accent : GetThemePalette(m_theme).icon);
        if (ImGui::IsItemHovered()) ShowTooltip(labels[i]);
        if (ImGui::IsItemClicked()) m_canvas->SetNumberSizeIndex(i);
    }
}

// 渲染预设颜色色块。
void SubToolbar::RenderColorControls(ImDrawList* drawList, const Metrics& metrics) {
    ImU32 currentColor = m_canvas->GetSelectedLayerColor();

    for (int i = 0; i < Canvas::PRESET_COLOR_COUNT; i++) {
        ImVec2 p_min = ImGui::GetCursorScreenPos();
        ImVec2 squareMin(p_min.x + (metrics.colorItemSize - metrics.colorSquareSize) / 2.0f,
                         p_min.y + (metrics.colorItemSize - metrics.colorSquareSize) / 2.0f);
        ImVec2 squareMax(squareMin.x + metrics.colorSquareSize, squareMin.y + metrics.colorSquareSize);
        ImU32 presetColor = Canvas::PRESET_COLORS[i];
        bool isSelected = (currentColor == presetColor);

        ImGui::InvisibleButton(("color_" + std::to_string(i)).c_str(), ImVec2(metrics.colorItemSize, metrics.colorItemSize));

        if (isSelected || ImGui::IsItemHovered()) {
            drawList->AddRectFilled(
                ImVec2(p_min.x - 2.0f * m_uiScale, p_min.y - 2.0f * m_uiScale),
                ImVec2(p_min.x + metrics.colorItemSize + 2.0f * m_uiScale,
                       p_min.y + metrics.colorItemSize + 2.0f * m_uiScale),
                GetThemePalette(m_theme).hoverBackground,
                1.0f * m_uiScale);
        }

        drawList->AddRectFilled(squareMin, squareMax, presetColor, 1.0f * m_uiScale);
        // 亮色主题下为白色色块描边，避免与工具栏背景融为一体。
        if (m_theme == Theme::Light && presetColor == Colors::PresetWhite) {
            drawList->AddRect(squareMin, squareMax, GetThemePalette(m_theme).border,
                              1.0f * m_uiScale, 0, 1.0f * m_uiScale);
        }

        if (ImGui::IsItemClicked()) {
            m_canvas->SetSelectedLayerColor(presetColor);
        }

        if (i < Canvas::PRESET_COLOR_COUNT - 1) {
            ImGui::SameLine(0, metrics.itemSpacing);
        }
    }
}

// 按子工具栏样式显示悬浮提示。
void SubToolbar::ShowTooltip(const char* text) const {
    const float s = m_uiScale;
    ImGui::PushStyleColor(ImGuiCol_PopupBg, ImGui::ColorConvertU32ToFloat4(GetThemePalette(m_theme).tooltipBackground));
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(GetThemePalette(m_theme).tooltipText));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(3.0f * s, 3.0f * s));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 2.0f * s);
    if (m_tooltipFont) ImGui::PushFont(m_tooltipFont);
    if (ImGui::BeginTooltip()) {
        ImGui::Text("%s", text);
        ImGui::EndTooltip();
    }
    if (m_tooltipFont) ImGui::PopFont();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

} // namespace shotlet
