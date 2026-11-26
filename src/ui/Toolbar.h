#pragma once
#include "i18n/Localization.h"
#include "Theme.h"
#include "utils/WindowChrome.h"
#include "ToolbarIcons.h"

#include <functional>
#include <memory>
#include <cstdint>
#include <vector>
#include <imgui.h>
#include <imgui_internal.h>

struct SDL_Renderer;
namespace shotlet {

class Canvas;
class SoftwareIconCache;

class Toolbar {
public:
    // 滚动截图入口开关；置 false 可隐藏入口，回调与实现保留。
    static constexpr bool ShowScrollCaptureButton = true;
    static constexpr int ScrollCaptureWidth = 216;
    static constexpr int FloatingHeight = 45;
    static constexpr float FloatingRounding = 8.0f;
    static constexpr float IconSize = 25.0f;
    static constexpr float IconStrokeWidth = 1.0f;
    static constexpr int ScrollCaptureHeight = FloatingHeight;
    static constexpr int ScrollCaptureButtonInsetY = 4;
    static constexpr int ScrollCaptureButtonHeight = ScrollCaptureHeight - 2 * ScrollCaptureButtonInsetY;
    static constexpr int ScrollCaptureButtonStep = 52;
    struct Bitmap {
        int width = 0;
        int height = 0;
        std::vector<uint8_t> rgba;
    };
    // 按物理像素倍率生成直通 RGBA 位图，布局尺寸仍以逻辑坐标计算。
    static Bitmap MakeScrollCaptureBitmap(float scale = 1.0f, Theme theme = Theme::Dark);
    using ConfirmCallback = std::function<void()>;
    using CancelCallback = std::function<void()>;
    using SaveCallback = std::function<void()>;
    using PinCallback = std::function<void()>;
    using OcrCallback = std::function<void()>;

    Toolbar() = default;

    void SetTheme(Theme theme) { m_theme = theme; }
    void SetLanguage(Language language) { m_language = language; }
    void SetCanvas(Canvas* canvas) { m_canvas = canvas; }
#if defined(SHOTLET_PLATFORM_LINUX)
    void SetSoftwareRenderer(SDL_Renderer* renderer);
#endif
    void SetConfirmCallback(ConfirmCallback cb) { m_confirmCallback = std::move(cb); }
    void SetCancelCallback(CancelCallback cb) { m_cancelCallback = std::move(cb); }
    void SetSaveCallback(SaveCallback cb) { m_saveCallback = std::move(cb); }
    void SetPinCallback(PinCallback cb) { m_pinCallback = std::move(cb); }
    void SetOcrCallback(OcrCallback cb) { m_ocrCallback = std::move(cb); }
    void SetScrollCallback(std::function<void()> cb) { m_scrollCallback = std::move(cb); }
    void SetRecordCallback(std::function<void()> cb) { m_recordCallback = std::move(cb); }
    void SetEditorMode(bool editor) { m_editorMode = editor; }
    bool IsEditorMode() const { return m_editorMode; }
    enum class EditorAction { ZoomOut, ZoomIn, ActualSize, Rotate, Crop };
    void SetEditorActionCallback(std::function<void(EditorAction)> cb) { m_editorActionCallback = std::move(cb); }
    void SetEditorActionsEnabled(bool enabled) { m_editorActionsEnabled = enabled; }
    void SetEditorCropping(bool cropping) { m_editorCropping = cropping; }
    // 编辑器标题栏使用窗口逻辑尺寸，Retina 像素密度由渲染后端处理。
    // 高度与历史记录、设置、置顶、OCR 窗口共用 WindowChrome 统一常量。
    static constexpr float EditorHeight = WindowChrome::TitleBarHeight;
    // ImGui 会取整窗口边界，非整数 DPI 下让视口使用同一个边界，避免半像素接缝。
    float GetHeight() const { return m_editorMode ? ImFloor(EditorHeight * m_uiScale) : FloatingHeight * m_uiScale; }
    // 原生窗口按钮占据的逻辑宽度；只影响编辑器布局，不缩放图标或线宽。
    void SetEditorLeadingInset(float inset) { m_editorLeadingInset = ImMax(0.0f, inset); }
    bool IsEditorDragArea(ImVec2 point) const;
    // 窗口控制动作由共享 WindowChrome 定义，macOS 继续使用原生窗口按钮。
    using WindowAction = WindowChrome::WindowAction;
    // 提供回调时启用右侧窗口控制组，macOS 继续使用原生窗口按钮。
    void SetWindowActionCallback(std::function<void(WindowAction)> cb) { m_windowActionCallback = std::move(cb); }
    void SetWindowMaximized(bool maximized) { m_windowMaximized = maximized; }
    void SetTooltipFont(ImFont* font) { m_tooltipFont = font; }
    void SetUIScale(float scale) { m_uiScale = scale > 0.0f ? scale : 1.0f; }

    ImVec2 GetSelectedToolPos() const { return m_selectedToolPos; }
    float GetSelectedIconSize() const { return m_renderedIconSize; }
    bool HasSelectedToolPos() const { return m_hasSelectedToolPos; }

    ImRect GetWindowRect() const { return m_windowRect; }

    void Render();
    // 单独调用，使窗口控制按钮在裁剪等标注工具被禁用时仍可操作。
    void RenderWindowControls();

private:
    void DrawToolbarIcon(ImDrawList* drawList, ImVec2 position, float size, ToolbarIcons::IconType type,
                         ImU32 color, float thickness);
#if defined(SHOTLET_PLATFORM_LINUX)
    std::shared_ptr<SoftwareIconCache> m_softwareIcons;
#endif
    void RecordEditorControl();
    float GetWindowControlsWidth() const;
    Theme m_theme = Theme::Dark;
    Language m_language = Language::English;
    Canvas* m_canvas = nullptr;
    ConfirmCallback m_confirmCallback;
    CancelCallback m_cancelCallback;
    SaveCallback m_saveCallback;
    PinCallback m_pinCallback;
    OcrCallback m_ocrCallback;
    std::function<void()> m_scrollCallback;
    std::function<void()> m_recordCallback;
    ImFont* m_tooltipFont = nullptr;
    float m_uiScale = 1.0f;
    float m_renderedIconSize = IconSize;
    bool m_editorMode = false;
    float m_editorLeadingInset = 0;
    std::vector<ImRect> m_editorControlRects;
    bool m_editorActionsEnabled = true;
    bool m_editorCropping = false;
    std::function<void(EditorAction)> m_editorActionCallback;
    std::function<void(WindowAction)> m_windowActionCallback;
    bool m_windowMaximized = false;
    ImVec2 m_selectedToolPos = ImVec2(0, 0);
    bool m_hasSelectedToolPos = false;
    ImRect m_windowRect = ImRect(1, 1, 0, 0);
};

} // namespace shotlet
