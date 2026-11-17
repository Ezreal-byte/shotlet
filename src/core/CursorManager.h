#pragma once
#include "Layer.h"
#include <imgui.h>
#include <SDL3/SDL.h>
#include <vector>
#include <memory>

namespace shotlet {

// 绘图工具类型（前向声明，实际定义在 Canvas.h）
enum class ToolType;

// 光标类型枚举 — 每帧由 CursorManager 计算
enum class CursorType {
    Arrow,          // 默认箭头
    Crosshair,      // 绘图工具选中 + 鼠标在选区内
    MosaicBrush,    // 马赛克笔刷（ImGui None + 自绘圆）
    ResizeNWSE,     // 对角线缩放
    ResizeNESW,     // 反对角线缩放
    ResizeNS,       // 垂直缩放
    ResizeEW,       // 水平缩放
    ResizeAll,      // 图层拖拽移动
    Hand,           // 未选中图层悬停
};

// 光标决策所需的上下文 — 由 Canvas 每帧填充
struct CursorContext {
    ImVec2 mousePos;
    ToolType currentTool;
    bool hasSelection = false;
    ImRect selectionRect;
    bool textInputActive = false;
    bool mouseDown = false;
    Layer* selectedLayer = nullptr;
    const std::vector<std::unique_ptr<Layer>>* layers = nullptr;
    ResizeHandle selectionHandle = ResizeHandle::None;  // 选区手柄检测结果
    float padding = 5.0f;       // 内边距（与 Canvas::padding 一致）
    float textFrameScale = 1.0f; // TextLayer 边框/热区 padding 缩放
    float mosaicBrushRadius = 10.0f;  // 马赛克笔刷半径（逻辑坐标）
    float uiScale = 1.0f;
};

// 光标管理器 — 统一管理光标决策、ImGui 设置、SDL 应用、自定义视觉
// 生命周期由 ScreenOverlay 管理，通过 Canvas::SetCursorManager 传入
class CursorManager {
public:
    CursorManager();
    ~CursorManager();

    // 计算光标类型 + 设置 ImGui 光标（马赛克 → None，其他 → 不干预）
    // 在 Canvas::Render 内调用（ImGui::NewFrame 已执行）
    CursorType UpdateAndSetImGui(const CursorContext& ctx);

    // 绘制自定义光标视觉（马赛克笔刷圆）
    // 在 Canvas::Render 的渲染部分末尾调用
    static void RenderCustomVisual(ImDrawList* drawList, const CursorContext& ctx, CursorType type);
    static float GetMosaicBrushVisualRadius(float logicalRadius, float uiScale);
    static ImU32 GetMosaicBrushFillColor();
    static ImU32 GetMosaicBrushOutlineColor();

    // 应用 SDL 系统光标（十字光标覆盖 ImGui 设置）
    // 在 ScreenOverlay::RenderFrame 的 ImGui::Render() 之后调用，仅活跃 overlay
    void ApplySDL(CursorType type) const;

    // SDL 光标资源管理
    void InitSDLResources();
    void CleanupSDLResources();

private:
    // 将 ResizeHandle 映射到 CursorType
    static CursorType ResizeHandleToCursorType(ResizeHandle handle);

    // 将 CursorType 映射到 ImGuiMouseCursor（不可 static_cast，两枚举值顺序不同）
    static ImGuiMouseCursor CursorTypeToImGuiMouseCursor(CursorType type);

    SDL_Cursor* m_crosshairCursor = nullptr;
    CursorType m_currentType = CursorType::Arrow;
};

} // namespace shotlet
