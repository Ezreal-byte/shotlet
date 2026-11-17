#include "Toolbar.h"
#include "ToolbarIcons.h"
#include "WindowChrome.h"
#include "core/Canvas.h"
#include "utils/Colors.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <algorithm>
#include <cmath>

namespace shotlet {

namespace EditorToolbarLayout {
    constexpr int IconCount = 18;
    constexpr int CaptureIconCount = 15;
    constexpr float HorizontalPadding = 10.0f;
    constexpr float ConfirmButtonWidth = 50.0f;
    constexpr float ConfirmButtonHeight = 20.0f;
    constexpr float ConfirmRightInset = 16.0f;
    constexpr float MinimumIconGap = 2.0f;
    constexpr float MaximumIconGap = 24.0f;
}

float Toolbar::GetWindowControlsWidth() const {
    return m_editorMode && m_windowActionCallback ? WindowChrome::WindowControlsWidth(m_uiScale, /*includeMaximize=*/true) : 0;
}

void Toolbar::RenderWindowControls() {
    // macOS 使用原生窗口按钮；没有动作回调时也不能生成会遮挡工具栏的控制组。
    if (GetWindowControlsWidth() <= 0) return;
    // 窗口控制组的绘制与命中由共享 WindowChrome 模块统一提供，编辑器仅保留薄委托。
    WindowChrome::RenderWindowControls("EditorWindowControls", GetHeight(), m_theme, m_language,
                                       m_uiScale, m_windowMaximized, /*includeMaximize=*/true,
                                       GetThemePalette(m_theme).editorToolbarBackground,
                                       m_tooltipFont, m_windowActionCallback);
}

void Toolbar::RecordEditorControl() {
    if (m_editorMode)
        m_editorControlRects.emplace_back(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
}

bool Toolbar::IsEditorDragArea(ImVec2 point) const {
    if (!m_editorMode || !m_windowRect.Contains(point) || point.x < m_editorLeadingInset)
        return false;
    return std::none_of(m_editorControlRects.begin(), m_editorControlRects.end(),
                        [point](const ImRect& rect) { return rect.Contains(point); });
}

Toolbar::Bitmap Toolbar::MakeScrollCaptureBitmap(float scale, Theme theme) {
    const auto& palette = GetThemePalette(theme);
    if (!std::isfinite(scale) || scale <= 0.0f) scale = 1.0f;
    scale = std::min(scale, 8.0f);
    // 每个输出像素使用 4×4 个采样点，平滑细线、斜线和圆角。
    constexpr int samples = 4;
    const int outputWidth = static_cast<int>(std::lround(ScrollCaptureWidth * scale));
    const int outputHeight = static_cast<int>(std::lround(ScrollCaptureHeight * scale));
    Bitmap bitmap;
    bitmap.width = std::max(1, outputWidth) * samples;
    bitmap.height = std::max(1, outputHeight) * samples;
    bitmap.rgba.resize(static_cast<size_t>(bitmap.width) * bitmap.height * 4);

    ImDrawListSharedData shared;
    shared.SetCircleTessellationMaxError(0.3f);
    ImDrawList draw(&shared);
    draw._ResetForNewFrame();
    draw.PushClipRect({0, 0}, {static_cast<float>(ScrollCaptureWidth), static_cast<float>(ScrollCaptureHeight)}, true);
    draw.AddRectFilled({0, 0}, {static_cast<float>(ScrollCaptureWidth), static_cast<float>(ScrollCaptureHeight)},
                       palette.panelBackground, FloatingRounding);
    constexpr float iconSize = IconSize;
    const auto iconPos = [](const int index) {
        return ImVec2(17.5f + index * ScrollCaptureButtonStep, (ScrollCaptureHeight - IconSize) / 2);
    };
    ToolbarIcons::DrawEditIcon(&draw, iconPos(0), iconSize, palette.icon, IconStrokeWidth);
    ToolbarIcons::DrawSaveIcon(&draw, iconPos(1), iconSize, palette.icon, IconStrokeWidth);
    ToolbarIcons::DrawCloseIcon(&draw, iconPos(2), iconSize, palette.danger, 1.5f);
    ToolbarIcons::DrawOkIcon(&draw, iconPos(3), iconSize, palette.confirm, 1.5f);

    for (auto& vertex : draw.VtxBuffer) {
        vertex.pos.x *= static_cast<float>(bitmap.width) / ScrollCaptureWidth;
        vertex.pos.y *= static_cast<float>(bitmap.height) / ScrollCaptureHeight;
    }

    // ImDrawList 的图标和圆角背景已三角化；将同一份绘图结果栅格化给原生窗口使用。
    const auto edge = [](const ImVec2& a, const ImVec2& b, const float x, const float y) {
        return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
    };
    for (int index = 0; index + 2 < draw.IdxBuffer.Size; index += 3) {
        const ImDrawVert& a = draw.VtxBuffer[draw.IdxBuffer[index]];
        ImDrawVert b = draw.VtxBuffer[draw.IdxBuffer[index + 1]];
        ImDrawVert c = draw.VtxBuffer[draw.IdxBuffer[index + 2]];
        float area = edge(a.pos, b.pos, c.pos.x, c.pos.y);
        if (area < 0) { std::swap(b, c); area = -area; }
        if (std::abs(area) < 0.001f) continue;
        const int left = std::max(0, static_cast<int>(std::floor(std::min({a.pos.x, b.pos.x, c.pos.x}))));
        const int top = std::max(0, static_cast<int>(std::floor(std::min({a.pos.y, b.pos.y, c.pos.y}))));
        const int right = std::min(bitmap.width, static_cast<int>(std::ceil(std::max({a.pos.x, b.pos.x, c.pos.x}))));
        const int bottom = std::min(bitmap.height, static_cast<int>(std::ceil(std::max({a.pos.y, b.pos.y, c.pos.y}))));
        for (int y = top; y < bottom; ++y) {
            for (int x = left; x < right; ++x) {
                const float w0 = edge(b.pos, c.pos, x + 0.5f, y + 0.5f) / area;
                const float w1 = edge(c.pos, a.pos, x + 0.5f, y + 0.5f) / area;
                const float w2 = 1.0f - w0 - w1;
                // 共享边只属于一个三角形，避免半透明背景重复混合或留下接缝。
                const auto inside = [&](const ImVec2& p, const ImVec2& q) {
                    const float e = edge(p, q, x + 0.5f, y + 0.5f);
                    return e > 0 || (e == 0 && (q.y < p.y || (q.y == p.y && q.x > p.x)));
                };
                if (!inside(b.pos, c.pos) || !inside(c.pos, a.pos) || !inside(a.pos, b.pos)) continue;
                const ImU32 colors[3] = {a.col, b.col, c.col};
                float src[4]{};
                for (int i = 0; i < 4; ++i) {
                    src[i] = (static_cast<float>((colors[0] >> (i * 8)) & 255) * w0 +
                              static_cast<float>((colors[1] >> (i * 8)) & 255) * w1 +
                              static_cast<float>((colors[2] >> (i * 8)) & 255) * w2) / 255.0f;
                }
                uint8_t* dst = bitmap.rgba.data() + (static_cast<size_t>(y) * bitmap.width + x) * 4;
                const float da = dst[3] / 255.0f;
                const float outA = src[3] + da * (1.0f - src[3]);
                if (outA <= 0) continue;
                for (int i = 0; i < 3; ++i) {
                    const float rgb = (src[i] * src[3] + dst[i] / 255.0f * da * (1.0f - src[3])) / outA;
                    dst[i] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(rgb * 255.0f)), 0, 255));
                }
                dst[3] = static_cast<uint8_t>(std::clamp(static_cast<int>(std::lround(outA * 255.0f)), 0, 255));
            }
        }
    }
    Bitmap result;
    result.width = bitmap.width / samples;
    result.height = bitmap.height / samples;
    result.rgba.resize(static_cast<size_t>(result.width) * result.height * 4);
    for (int y = 0; y < result.height; ++y) {
        for (int x = 0; x < result.width; ++x) {
            unsigned alpha = 0, rgb[3]{};
            for (int sy = 0; sy < samples; ++sy) {
                for (int sx = 0; sx < samples; ++sx) {
                    const auto* pixel = bitmap.rgba.data() +
                        (static_cast<size_t>(y * samples + sy) * bitmap.width + x * samples + sx) * 4;
                    alpha += pixel[3];
                    for (int i = 0; i < 3; ++i) rgb[i] += static_cast<unsigned>(pixel[i]) * pixel[3];
                }
            }
            // 在预乘 alpha 空间缩小，再还原直通 RGBA，避免透明边缘出现黑边。
            auto* pixel = result.rgba.data() + (static_cast<size_t>(y) * result.width + x) * 4;
            for (int i = 0; i < 3; ++i) pixel[i] = alpha ? (rgb[i] + alpha / 2) / alpha : 0;
            pixel[3] = (alpha + samples * samples / 2) / (samples * samples);
        }
    }
    return result;
}

void Toolbar::Render() {
    ImGuiIO& io = ImGui::GetIO();
    const float s = m_uiScale;
    const float screenMargin = 10.0f * s;
    const float selectionGap = 20.0f * s;

    m_hasSelectedToolPos = false;
    m_windowRect = ImRect(1, 1, 0, 0);  // 每帧重置，未渲染时保持无效
    m_editorControlRects.clear();

    if (!m_canvas || !m_canvas->HasSelection()) return;

    // 获取选区位置
    ImRect selection = m_canvas->GetSelection();

    // 工具栏参数
    float iconSize = IconSize * s;
    const float toolbarHeight = GetHeight();
    const float padding = EditorToolbarLayout::HorizontalPadding * s;
    const float leadingInset = m_editorMode ? m_editorLeadingInset : 0;
    const float editorConfirmReserve = (EditorToolbarLayout::ConfirmButtonWidth + EditorToolbarLayout::ConfirmRightInset) * s;
    const ImVec2 editorButtonSize(EditorToolbarLayout::ConfirmButtonWidth * s, EditorToolbarLayout::ConfirmButtonHeight * s);
    const int iconNum = (m_editorMode ? EditorToolbarLayout::IconCount : EditorToolbarLayout::CaptureIconCount) +
        (ShowScrollCaptureButton && m_scrollCallback ? 1 : 0) + (m_recordCallback ? 1 : 0);
    const int gapNum = iconNum + (m_editorMode ? 4 : 0);
    const float editorWidth = ImMax(1.0f, io.DisplaySize.x - GetWindowControlsWidth());
    const float availableWidth = ImMax(1.0f, m_editorMode ? editorWidth - leadingInset : io.DisplaySize.x - 2 * screenMargin);
    const float fixedWidth = 2 * padding + (m_editorMode ? 2.0f + editorConfirmReserve : 1.0f);
    float iconSpacing = ImClamp((availableWidth - fixedWidth - iconNum * iconSize) / gapNum,
                               EditorToolbarLayout::MinimumIconGap * s, (m_editorMode ? EditorToolbarLayout::MaximumIconGap : 20.0f) * s);
    // 窄窗口先压缩间距，再缩小图标；副工具栏使用实际图标尺寸定位。
    iconSize = ImMin(iconSize, ImMax(1.0f, (availableWidth - fixedWidth - gapNum * iconSpacing) / iconNum));
    m_renderedIconSize = iconSize;
    // 标题栏同时容纳图标和较矮的完成按钮，按较高的控件留白，避免产生隐藏滚动范围。
    const float verticalPadding = m_editorMode ? (toolbarHeight - ImMax(iconSize, editorButtonSize.y)) / 2.0f : padding;
    const float toolbarWidth = m_editorMode ? editorWidth : fixedWidth + iconNum * iconSize + gapNum * iconSpacing;

    auto clamp = [](float value, float minValue, float maxValue) {
        return value < minValue ? minValue : (value > maxValue ? maxValue : value);
    };

    // 计算工具栏位置：横向只限制在屏幕内，纵向始终优先放在选区外。
    const float minX = screenMargin;
    const float maxX = io.DisplaySize.x - toolbarWidth - screenMargin;
    float toolbarX = selection.GetCenter().x - toolbarWidth / 2;
    toolbarX = maxX >= minX ? clamp(toolbarX, minX, maxX) : minX;

    const float belowY = selection.Max.y + selectionGap;
    const float aboveY = selection.Min.y - selectionGap - toolbarHeight;
    const float maxY = io.DisplaySize.y - toolbarHeight - screenMargin;
    float toolbarY = belowY;

    if (belowY + toolbarHeight > io.DisplaySize.y - screenMargin) {
        if (aboveY >= screenMargin) {
            toolbarY = aboveY;
        } else {
            const float spaceBelow = io.DisplaySize.y - screenMargin - belowY;
            const float spaceAbove = aboveY + toolbarHeight - screenMargin;
            toolbarY = spaceAbove > spaceBelow ? aboveY : belowY;
        }
    }
    toolbarY = maxY >= screenMargin ? clamp(toolbarY, screenMargin, maxY) : screenMargin;

    if (m_editorMode) { toolbarX = 0; toolbarY = 0; }
    ImGui::SetNextWindowPos(ImVec2(toolbarX, toolbarY));
    ImGui::SetNextWindowSize(ImVec2(toolbarWidth, toolbarHeight));

    // 存储工具栏窗口矩形范围（供 ScreenOverlay 判断点击是否在工具栏区域内）
    m_windowRect = ImRect(toolbarX, toolbarY, toolbarX + toolbarWidth, toolbarY + toolbarHeight);

    constexpr ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, verticalPadding));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, m_editorMode ? 0.0f : FloatingRounding * s);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, m_editorMode ? ImGui::ColorConvertU32ToFloat4(GetThemePalette(m_theme).editorToolbarBackground) : ImGui::ColorConvertU32ToFloat4(GetThemePalette(m_theme).panelBackground));

    const bool cropping = m_editorMode && m_editorCropping;
    const ImU32 baseIconColor = cropping ? GetThemePalette(m_theme).disabledIcon : GetThemePalette(m_theme).icon;
    const ImU32 greenColor = GetThemePalette(m_theme).accent;
    const ImU32 selectedBgColor = GetThemePalette(m_theme).selectedBackground;
    const ImU32 disabledColor = GetThemePalette(m_theme).disabledIcon;

    if (ImGui::Begin("Toolbar", nullptr, flags)) {
        // 窄窗口压缩图标后仍保持垂直居中，避免 DPI 缩放后图标贴向工具栏上沿。
        if (m_editorMode) ImGui::SetCursorPos(ImVec2(leadingInset + padding, (toolbarHeight - iconSize) / 2.0f));
        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Tooltip 样式：内边距 3px，圆角 2px，字体 12pt
        // 必须在 BeginTooltip() 之前 push WindowRounding（tooltip 不走 PopupRounding，走 WindowRounding）
        auto showTooltip = [this, s](const char* text) {
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
        };

        // 工具图标绘制函数
        auto drawToolIcon = [&](const char* id, ToolType tool, ImU32 color, const char* tooltip) {
            ImVec2 p_min = ImGui::GetCursorScreenPos();
            ImVec2 p_max = ImVec2(p_min.x + iconSize, p_min.y + iconSize);

            bool isSelected = (!cropping && m_canvas && m_canvas->GetPropertyTool() == tool);

            if (isSelected) {
                drawList->AddRectFilled(p_min, p_max, selectedBgColor, 2.0f * s);
            }

            ImGui::InvisibleButton(id, ImVec2(iconSize, iconSize));
            RecordEditorControl();

            // 记录选中工具图标位置（供 SubToolbar 定位使用）
            if (isSelected) {
                m_selectedToolPos = p_min;
                m_hasSelectedToolPos = true;
            }

            ImU32 iconColor = isSelected ? greenColor : baseIconColor;
            ToolbarIcons::IconType iconType;
            switch (tool) {
                case ToolType::Rectangle:
                    iconType = ToolbarIcons::IconType::Rectangle;
                    break;
                case ToolType::Ellipse:
                    iconType = ToolbarIcons::IconType::Circle;
                    break;
                case ToolType::Arrow:
                    iconType = ToolbarIcons::IconType::Arrow;
                    break;
                case ToolType::Brush:
                    iconType = ToolbarIcons::IconType::Brush;
                    break;
                case ToolType::Number:
                    iconType = ToolbarIcons::IconType::Number;
                    break;
                case ToolType::Highlight:
                    iconType = ToolbarIcons::IconType::Highlight;
                    break;
                case ToolType::Watermark:
                    iconType = ToolbarIcons::IconType::Watermark;
                    break;
                default:
                    iconType = ToolbarIcons::IconType::Rectangle;
                    break;
            }
            ToolbarIcons::DrawIcon(drawList, p_min, iconSize, iconType, iconColor, IconStrokeWidth * s);

            if (ImGui::IsItemHovered()) {
                showTooltip(tooltip);
            }

            if (ImGui::IsItemClicked()) {
                if (m_canvas) {
                    if (m_canvas->GetTool() == tool) {
                        // toggle off 当前工具：不清除图层选中状态
                        m_canvas->SetTool(ToolType::None, false);
                    } else {
                        // 切换到其他工具：清除图层选中状态
                        m_canvas->SetTool(tool);
                    }
                }
            }
        };

        // 编辑器视图操作与裁剪连续排列；裁剪期间只保留裁剪开关和确认入口。
        if (m_editorMode) {
            const auto drawEditorAction = [&](const char* id, EditorAction action,
                                              ToolbarIcons::IconType icon, const char* tooltip) {
                const ImVec2 pos = ImGui::GetCursorScreenPos();
                const bool selected = action == EditorAction::Crop && m_editorCropping;
                ImGui::BeginDisabled(!m_editorActionsEnabled || (m_editorCropping && action != EditorAction::Crop));
                const bool clicked = ImGui::InvisibleButton(id, ImVec2(iconSize, iconSize));
                RecordEditorControl();
                if (selected || ImGui::IsItemHovered()) {
                    drawList->AddRectFilled(pos, ImVec2(pos.x + iconSize, pos.y + iconSize), selectedBgColor, 2.0f * s);
                }
                if (ImGui::IsItemHovered()) showTooltip(tooltip);
                // 落笔期间仅锁定操作，图标保持原色，避免每笔按下和松开时反复明暗切换。
                ToolbarIcons::DrawIcon(drawList, pos, iconSize, icon, selected ? greenColor : baseIconColor, IconStrokeWidth * s);
                if (clicked && m_editorActionCallback) m_editorActionCallback(action);
                ImGui::EndDisabled();
                ImGui::SameLine(0, iconSpacing);
            };
            drawEditorAction("zoom_out_btn", EditorAction::ZoomOut, ToolbarIcons::IconType::ZoomOut, Tr(m_language, TextId::ActionZoomOut));
            drawEditorAction("zoom_in_btn", EditorAction::ZoomIn, ToolbarIcons::IconType::ZoomIn, Tr(m_language, TextId::ActionZoomIn));
            drawEditorAction("actual_size_btn", EditorAction::ActualSize, ToolbarIcons::IconType::ActualSize, Tr(m_language, TextId::ActionActualSize));
            drawEditorAction("rotate_btn", EditorAction::Rotate, ToolbarIcons::IconType::Rotate, Tr(m_language, TextId::ActionRotate));
            drawEditorAction("crop_btn", EditorAction::Crop, ToolbarIcons::IconType::Crop,
                             Tr(m_language, m_editorCropping ? TextId::EditorCropCancel : TextId::EditorCrop));
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine(0, iconSpacing);
        }

        // 仅高亮当前裁剪模式，原标注工具的选择留在画布中，取消裁剪后可继续使用。
        ImGui::BeginDisabled(cropping);
        // 绘制工具图标
        drawToolIcon("rect_btn", ToolType::Rectangle, baseIconColor, Tr(m_language, TextId::ToolRectangle));
        ImGui::SameLine(0, iconSpacing);
        drawToolIcon("circle_btn", ToolType::Ellipse, baseIconColor, Tr(m_language, TextId::ToolEllipse));
        ImGui::SameLine(0, iconSpacing);
        drawToolIcon("arrow_btn", ToolType::Arrow, baseIconColor, Tr(m_language, TextId::ToolArrow));
        ImGui::SameLine(0, iconSpacing);
        drawToolIcon("brush_btn", ToolType::Brush, baseIconColor, Tr(m_language, TextId::ToolBrush));
        ImGui::SameLine(0, iconSpacing);

        // 马赛克图标
        {
            ImVec2 p_min = ImGui::GetCursorScreenPos();
            ImVec2 p_max = ImVec2(p_min.x + iconSize, p_min.y + iconSize);

            bool isSelected = (!cropping && m_canvas && m_canvas->GetPropertyTool() == ToolType::Mosaic);
            ImU32 iconColor = isSelected ? greenColor : baseIconColor;

            if (isSelected) {
                drawList->AddRectFilled(p_min, p_max, selectedBgColor, 2.0f * s);
            }

            ImGui::InvisibleButton("mosaic_btn", ImVec2(iconSize, iconSize));
            RecordEditorControl();
            if (isSelected) {
                m_selectedToolPos = p_min;
                m_hasSelectedToolPos = true;
            }
            ToolbarIcons::DrawMosaicIcon(drawList, p_min, iconSize, iconColor);

            if (ImGui::IsItemHovered()) {
                showTooltip(Tr(m_language, TextId::ToolMosaic));
            }

            if (ImGui::IsItemClicked()) {
                if (m_canvas) {
                    if (m_canvas->GetTool() == ToolType::Mosaic) {
                        m_canvas->SetTool(ToolType::None, false);
                    } else {
                        m_canvas->SetTool(ToolType::Mosaic);
                    }
                }
            }
        }
        ImGui::SameLine(0, iconSpacing);

        drawToolIcon("highlight_btn", ToolType::Highlight, baseIconColor, Tr(m_language, TextId::ToolHighlight));
        ImGui::SameLine(0, iconSpacing);
        drawToolIcon("watermark_btn", ToolType::Watermark, baseIconColor, Tr(m_language, TextId::ToolWatermark));
        ImGui::SameLine(0, iconSpacing);

        // 文字图标
        {
            ImVec2 p_min = ImGui::GetCursorScreenPos();
            ImVec2 p_max = ImVec2(p_min.x + iconSize, p_min.y + iconSize);

            bool isSelected = (!cropping && m_canvas && m_canvas->GetPropertyTool() == ToolType::Text);
            ImU32 iconColor = isSelected ? greenColor : baseIconColor;

            if (isSelected) {
                drawList->AddRectFilled(p_min, p_max, selectedBgColor, 2.0f * s);
            }

            ImGui::InvisibleButton("text_btn", ImVec2(iconSize, iconSize));
            RecordEditorControl();
            if (isSelected) {
                m_selectedToolPos = p_min;
                m_hasSelectedToolPos = true;
            }
            ToolbarIcons::DrawTextIcon(drawList, p_min, iconSize, iconColor, IconStrokeWidth * s);

            if (ImGui::IsItemHovered()) {
                showTooltip(Tr(m_language, TextId::ToolText));
            }

            if (ImGui::IsItemClicked()) {
                if (m_canvas) {
                    if (m_canvas->GetTool() == ToolType::Text) {
                        m_canvas->SetTool(ToolType::None, false);
                    } else {
                        m_canvas->SetTool(ToolType::Text);
                    }
                }
            }
        }
        ImGui::SameLine(0, iconSpacing);

        drawToolIcon("number_btn", ToolType::Number, baseIconColor, Tr(m_language, TextId::ToolNumber));
        ImGui::SameLine(0, iconSpacing);

        // OCR 识别图标
        {
            const ImVec2 p_min = ImGui::GetCursorScreenPos();
            const auto p_max = ImVec2(p_min.x + iconSize, p_min.y + iconSize);
            ImGui::InvisibleButton("ocr_btn", ImVec2(iconSize, iconSize));
            RecordEditorControl();
            if (ImGui::IsItemHovered()) {
                drawList->AddRectFilled(p_min, p_max, selectedBgColor, 2.0f * s);
                showTooltip(Tr(m_language, TextId::ActionOcr));
            }
            ToolbarIcons::DrawOcrIcon(drawList, p_min, iconSize, baseIconColor, IconStrokeWidth * s);
            if (ImGui::IsItemClicked()) {
                if (m_ocrCallback) m_ocrCallback();
            }
        }
        ImGui::SameLine(0, iconSpacing);

        // 滚动截图入口置于 OCR 后，仅支持的平台提供回调。
        if (ShowScrollCaptureButton && m_scrollCallback) {
            const ImVec2 p_min = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("scroll_capture", ImVec2(iconSize, iconSize));
            RecordEditorControl();
            ToolbarIcons::DrawScrollIcon(drawList, p_min, iconSize, baseIconColor, IconStrokeWidth * s);
            if (ImGui::IsItemHovered()) showTooltip(Tr(m_language, TextId::ActionScroll));
            if (ImGui::IsItemClicked()) m_scrollCallback();
            ImGui::SameLine(0, iconSpacing);
        }

        if (m_recordCallback) {
            const ImVec2 position = ImGui::GetCursorScreenPos();
            ImGui::InvisibleButton("record_region", ImVec2(iconSize, iconSize));
            ToolbarIcons::DrawIcon(drawList, position, iconSize, ToolbarIcons::IconType::Record,
                baseIconColor, IconStrokeWidth * s);
            if (ImGui::IsItemHovered()) showTooltip(Tr(m_language, TextId::ActionRecord));
            if (ImGui::IsItemClicked()) m_recordCallback();
            ImGui::SameLine(0, iconSpacing);
        }

        // 撤回图标
        {
            ImVec2 p_min = ImGui::GetCursorScreenPos();

            bool canUndo = (m_canvas && m_canvas->CanUndo());
            ImU32 iconColor = canUndo ? baseIconColor : disabledColor;

            ImGui::InvisibleButton("undo_btn", ImVec2(iconSize, iconSize));
            RecordEditorControl();
            ToolbarIcons::DrawUndoIcon(drawList, p_min, iconSize, iconColor, IconStrokeWidth * s);

            if (ImGui::IsItemHovered()) {
                showTooltip(Tr(m_language, TextId::ActionUndo));
            }

            if (ImGui::IsItemClicked() && canUndo) {
                if (m_canvas) {
                    m_canvas->Undo();
                }
            }
        }
        ImGui::SameLine(0, iconSpacing);

        // 分隔线
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine(0, iconSpacing);

        // 置顶图标（分隔线后、保存前）
        {
            ImVec2 p_min = ImGui::GetCursorScreenPos();
            ImVec2 p_max = ImVec2(p_min.x + iconSize, p_min.y + iconSize);
            ImGui::InvisibleButton("pin_btn", ImVec2(iconSize, iconSize));
            RecordEditorControl();
            if (ImGui::IsItemHovered()) {
                drawList->AddRectFilled(p_min, p_max, selectedBgColor, 2.0f * s);
                showTooltip(Tr(m_language, TextId::ActionPin));
            }

            ToolbarIcons::DrawPinIcon(drawList, p_min, iconSize, baseIconColor, IconStrokeWidth * s);

            if (ImGui::IsItemClicked()) {
                if (m_pinCallback) m_pinCallback();
            }
        }
        ImGui::SameLine(0, iconSpacing);

        // 保存图标
        {
            ImVec2 p_min = ImGui::GetCursorScreenPos();
            ImVec2 p_max = ImVec2(p_min.x + iconSize, p_min.y + iconSize);
            ImGui::InvisibleButton("save_btn", ImVec2(iconSize, iconSize));
            RecordEditorControl();
            if (ImGui::IsItemHovered()) {
                drawList->AddRectFilled(p_min, p_max, selectedBgColor, 2.0f * s);
                showTooltip(Tr(m_language, TextId::ActionSaveImage));
            }

            ToolbarIcons::DrawSaveIcon(drawList, p_min, iconSize, baseIconColor, IconStrokeWidth * s);

            if (ImGui::IsItemClicked()) {
                if (m_saveCallback) m_saveCallback();
            }
        }
        ImGui::SameLine(0, iconSpacing);

        // 独立编辑窗口使用系统关闭按钮。
        if (!m_editorMode) {
            ImVec2 p_min = ImGui::GetCursorScreenPos();
            ImVec2 p_max = ImVec2(p_min.x + iconSize, p_min.y + iconSize);
            ImGui::InvisibleButton("close_btn", ImVec2(iconSize, iconSize));

            if (ImGui::IsItemHovered()) {
                drawList->AddRectFilled(p_min, p_max, selectedBgColor, 2.0f * s);
                showTooltip(Tr(m_language, TextId::ActionExit));
            }

            ImU32 closeColor = GetThemePalette(m_theme).danger;
            ToolbarIcons::DrawCloseIcon(drawList, p_min, iconSize, closeColor, 1.5f * s);
            if (ImGui::IsItemClicked()) {
                if (m_cancelCallback) m_cancelCallback();
            }
        }
        ImGui::SameLine(0, iconSpacing);

        ImGui::EndDisabled();

        // 裁剪使用同一个顶部完成入口，回调由编辑器按当前模式决定确认裁剪或复制结果。
        if (m_editorMode) {
            ImGui::BeginDisabled(m_editorCropping && !m_editorActionsEnabled);
            ImGui::SetCursorPosX(ImMax(ImGui::GetCursorPosX(), toolbarWidth - editorConfirmReserve));
            ImGui::SetCursorPosY((toolbarHeight - editorButtonSize.y) / 2.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, ImGui::ColorConvertU32ToFloat4(GetThemePalette(m_theme).confirmButton));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::ColorConvertU32ToFloat4(GetThemePalette(m_theme).confirmButtonHovered));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::ColorConvertU32ToFloat4(GetThemePalette(m_theme).confirmButtonActive));
            if (m_tooltipFont) ImGui::PushFont(m_tooltipFont);
            ImGui::PushStyleColor(ImGuiCol_Text, Colors::White);
            // 固定小按钮的文字按实际行高居中，避免默认上下内边距挤偏文字。
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, 0));
            if (ImGui::Button((std::string(Tr(m_language, TextId::ActionDone)) + "###confirm_capture").c_str(), editorButtonSize) && m_confirmCallback) m_confirmCallback();
            ImGui::PopStyleVar();
            RecordEditorControl();
            if (ImGui::IsItemHovered())
                showTooltip(Tr(m_language, m_editorCropping ? TextId::EditorCropApply : TextId::ActionCopyDone));
            if (m_tooltipFont) ImGui::PopFont();
            ImGui::PopStyleColor(4);
            ImGui::EndDisabled();
        } else {
            ImVec2 p_min = ImGui::GetCursorScreenPos();
            ImVec2 p_max = ImVec2(p_min.x + iconSize, p_min.y + iconSize);
            ImGui::InvisibleButton("ok_btn", ImVec2(iconSize, iconSize));

            if (ImGui::IsItemHovered()) {
                drawList->AddRectFilled(p_min, p_max, selectedBgColor, 2.0f * s);
                showTooltip(Tr(m_language, TextId::ActionDone));
            }

            ImU32 okColor = GetThemePalette(m_theme).confirm;
            ToolbarIcons::DrawOkIcon(drawList, p_min, iconSize, okColor, 1.5f * s);
            if (ImGui::IsItemClicked()) {
                if (m_confirmCallback) m_confirmCallback();
            }
        }
    }
    ImGui::End();

    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor();
}

} // namespace shotlet
