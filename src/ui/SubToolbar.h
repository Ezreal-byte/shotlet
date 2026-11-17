#pragma once
#include "i18n/Localization.h"
#include "Theme.h"
#include "core/CaptureDocument.h"

#include <imgui.h>
#include <imgui_internal.h>

namespace shotlet {

class Canvas;
class Toolbar;
enum class ToolType;

class SubToolbar {
public:
    void SetTheme(Theme theme) { m_theme = theme; }
    void SetLanguage(Language language) { m_language = language; }
    void SetCanvas(Canvas* canvas);
    void SetToolbar(Toolbar* toolbar) { m_toolbar = toolbar; }
    void SetTooltipFont(ImFont* font) { m_tooltipFont = font; }
    void SetUIScale(float scale) { m_uiScale = scale > 0.0f ? scale : 1.0f; }

    // 渲染子工具栏入口，负责组织配置、布局和控件组绘制。
    void Render();

    ImRect GetWindowRect() const { return m_windowRect; }

private:
    struct Options {
        ToolType tool;
        bool hasSelectedLayer = false;
        bool showFill = false;
        bool showThickness = false;
        bool showFontSize = false;
        bool showNumberSize = false;
        bool showHighlight = false;
        bool showWatermarkInput = false;
        bool showColors = true;
    };

    struct Metrics {
        float scale = 1.0f;
        float transparencyLabelWidth = 0;
        float transparencySliderWidth = 0;
        float transparencyValueWidth = 0;
        float watermarkInputWidth = 0;
        float padding = 0.0f;
        float itemSpacing = 0.0f;
        float dotRadiusSmall = 0.0f;
        float dotRadiusMedium = 0.0f;
        float dotRadiusLarge = 0.0f;
        float fillIconSize = 0.0f;
        float fillItemSize = 0.0f;
        float fontSizeItemSize = 0.0f;
        float colorSquareSize = 0.0f;
        float colorItemSize = 0.0f;
        float dividerSpacing = 0.0f;
        float arrowHeight = 0.0f;
        float arrowWidth = 0.0f;
        float height = 0.0f;
    };

    struct Layout {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        float selectedCenterX = 0.0f;
        bool placeAboveAnchor = false;
    };

    // 根据当前工具生成子工具栏需要显示的控件配置。
    bool BuildOptions(Options& options) const;
    void RenderNumberSizeControls(ImDrawList* drawList, const Metrics& metrics);
    void RenderHighlightControls(ImDrawList* drawList, const Metrics& metrics);
    // 高亮与水印共用字体、颜色、细滑轨和圆形抓手，只分别读写各自的百分比。
    bool RenderTransparencyControl(const char* id, TextId label, int& percent, int minimum, int maximum, const Metrics& metrics);
    // 渲染水印工具的文字输入框（默认焦点，最多 10 个字符）。
    void RenderWatermarkInput(const Metrics& metrics);
    // 将 UI 缩放换算成子工具栏中复用的尺寸参数。
    Metrics BuildMetrics() const;
    // 计算子工具栏相对主工具栏选中图标的窗口位置和朝向。
    bool CalculateLayout(const Options& options, const Metrics& metrics, Layout& layout) const;
    // 将计算后的布局应用到 ImGui 窗口，并更新点击命中的窗口矩形。
    void ApplyWindowLayout(const Layout& layout, const Metrics& metrics);
    // 绘制子工具栏背景边框和指向主工具栏图标的三角形箭头。
    void RenderFrameAndArrow(ImDrawList* drawList, const Layout& layout, const Metrics& metrics) const;
    // 渲染当前工具对应的全部子控件组。
    void RenderControls(ImDrawList* drawList, const Options& options, const Metrics& metrics);
    // 渲染矩形/椭圆工具的描边粗细与填充互斥选择。
    void RenderStrokeFillControls(ImDrawList* drawList, const Options& options, const Metrics& metrics);
    // 渲染箭头和画笔工具的描边粗细选择。
    void RenderThicknessControls(ImDrawList* drawList, const Options& options, const Metrics& metrics);
    // 渲染马赛克工具的笔刷大小选择。
    void RenderMosaicBrushControls(ImDrawList* drawList, const Metrics& metrics);
    // 渲染文字工具的字号选择。
    void RenderFontSizeControls(ImDrawList* drawList, const Metrics& metrics);
    // 在属性控件与颜色控件之间按需渲染分割线。
    void RenderDivider(const Options& options, const Metrics& metrics) const;
    // 渲染预设颜色色块。
    void RenderColorControls(ImDrawList* drawList, const Metrics& metrics);
    // 按子工具栏样式显示悬浮提示。
    void ShowTooltip(const char* text) const;

    Theme m_theme = Theme::Dark;
    Language m_language = Language::English;
    Canvas* m_canvas = nullptr;
    Toolbar* m_toolbar = nullptr;
    ImFont* m_tooltipFont = nullptr;
    float m_uiScale = 1.0f;
    ImRect m_windowRect = ImRect(1, 1, 0, 0);
    // 水印输入框状态：进入水印工具时同步画布文字并请求焦点（10 字符 * 4 字节 + 终止符）
    ToolType m_prevTool{};
    bool m_watermarkFocusPending = false;
    char m_watermarkBuffer[WatermarkSettings::MaxTextBytes + 1] = {};
};

} // namespace shotlet
