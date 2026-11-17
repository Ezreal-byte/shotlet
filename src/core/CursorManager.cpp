#include "utils/Colors.h"
#include "CursorManager.h"
#include "core/Canvas.h"      // ToolType 定义
#include "utils/Logger.h"
#include <algorithm>
#include <cmath>

namespace shotlet {

    CursorManager::CursorManager() = default;

    CursorManager::~CursorManager() {
        CleanupSDLResources();
    }

    void CursorManager::InitSDLResources() {
        m_crosshairCursor = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
        if (!m_crosshairCursor) {
            Logger::Warning("[CursorManager] Failed to create crosshair cursor");
        }
    }

    void CursorManager::CleanupSDLResources() {
        if (m_crosshairCursor) {
            SDL_DestroyCursor(m_crosshairCursor);
            m_crosshairCursor = nullptr;
        }
    }

    // ─── 核心决策逻辑 ───
    // 按优先级从高到低检查，返回最匹配的 CursorType
    // 同时设置 ImGui 光标（仅 MosaicBrush 需要 ImGuiMouseCursor_None）

    CursorType CursorManager::UpdateAndSetImGui(const CursorContext& ctx) {
        m_currentType = CursorType::Arrow;

        // 1. 文本输入活跃 → 默认箭头（文本框有自己的光标）
        if (ctx.textInputActive) {
            return m_currentType;
        }

        // 2. 有选区且鼠标不在选区内 → 恢复箭头光标
        // 例外：选区手柄在边界附近，保留手柄光标（拖拽时不检查，避免中途切换光标）
        const bool mouseInSelection = ctx.hasSelection && ctx.selectionRect.Contains(ctx.mousePos);
        if (ctx.hasSelection && !mouseInSelection
            && ctx.selectionHandle == ResizeHandle::None && !ctx.mouseDown) {
            return m_currentType;
        }

        // 3. 马赛克笔刷光标 — ImGuiMouseCursor_None + 自绘圆
        bool isMosaicTool = (ctx.currentTool == ToolType::Mosaic);

        if (isMosaicTool && mouseInSelection) {
            m_currentType = CursorType::MosaicBrush;
            ImGui::SetMouseCursor(ImGuiMouseCursor_None);
            return m_currentType;
        }

        // 4. 选中图层的手柄检测
        if (ctx.selectedLayer) {
            ResizeHandle handle = ctx.selectedLayer->HitTestResizeHandle(ctx.mousePos);
            if (handle != ResizeHandle::None) {
                m_currentType = ResizeHandleToCursorType(handle);
                ImGui::SetMouseCursor(CursorTypeToImGuiMouseCursor(m_currentType));
                return m_currentType;
            }
            if (auto* textLayer = dynamic_cast<TextLayer*>(ctx.selectedLayer)) {
                if (textLayer->GetFrameBounds(ctx.textFrameScale).Contains(ctx.mousePos)) {
                    m_currentType = CursorType::ResizeAll;
                    ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                    return m_currentType;
                }
                return m_currentType;
            }
            // 选中图层边框附近 → ResizeAll（拖拽移动）
            float threshold = ctx.selectedLayer->GetThickness() * 0.5f + ctx.padding;
            if (ctx.selectedLayer->IsPointNearBorder(ctx.mousePos, threshold)) {
                if (dynamic_cast<MosaicLayer*>(ctx.selectedLayer)) {
                    m_currentType = CursorType::Hand;
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    return m_currentType;
                }
                m_currentType = CursorType::ResizeAll;
                ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                return m_currentType;
            }
        }

        // 5. 未选中图层边框检测（从后往前遍历，上层优先）
        if (ctx.layers) {
            for (int i = static_cast<int>(ctx.layers->size()) - 1; i >= 0; i--) {
                Layer* layer = ctx.layers->at(i).get();
                if(dynamic_cast<MosaicLayer*>(layer) && ctx.currentTool!=ToolType::None)continue;
                if (dynamic_cast<HighlightLayer*>(layer) && ctx.currentTool != ToolType::None && ctx.currentTool != ToolType::Highlight) continue;
                // TextLayer 用 padding 扩大的区域检测
                if (layer->SupportsFontSize()) {
                    auto* textLayer = static_cast<TextLayer*>(layer);
                    ImRect bounds = textLayer->GetFrameBounds(ctx.textFrameScale);
                    if (bounds.Contains(ctx.mousePos)) {
                        m_currentType = CursorType::ResizeAll;
                        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
                        return m_currentType;
                    }
                }
                float threshold = layer->GetThickness() * 0.5f + ctx.padding;
                if (layer->IsPointNearBorder(ctx.mousePos, threshold)) {
                    m_currentType = CursorType::Hand;
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    return m_currentType;
                }
            }
        }

        // 6. 选区手柄检测（无图层选中、未拖拽时）
        if (ctx.hasSelection && !ctx.selectedLayer && !ctx.mouseDown) {
            if (ctx.selectionHandle != ResizeHandle::None) {
                m_currentType = ResizeHandleToCursorType(ctx.selectionHandle);
                ImGui::SetMouseCursor(CursorTypeToImGuiMouseCursor(m_currentType));
                return m_currentType;
            }
        }

        // 7. 绘图工具选中 + 鼠标在选区内 → 十字光标
        bool isDrawingTool = (ctx.currentTool != ToolType::None && ctx.currentTool != ToolType::Mosaic);
        if (isDrawingTool && mouseInSelection) {
            m_currentType = CursorType::Crosshair;
            // 十字光标通过 SDL 系统光标实现，ImGui 侧不干预
            return m_currentType;
        }

        // 8. 默认 Arrow
        return m_currentType;
    }

    // ─── 自定义视觉渲染 ───
    float CursorManager::GetMosaicBrushVisualRadius(float logicalRadius, float uiScale) {
        const float scale = std::isfinite(uiScale) ? std::max(uiScale, 1.0f) : 1.0f;
        return logicalRadius * scale;
    }

    ImU32 CursorManager::GetMosaicBrushFillColor() {
        return Colors::CursorFill;
    }

    ImU32 CursorManager::GetMosaicBrushOutlineColor() {
        return Colors::CursorBorder;
    }

    void CursorManager::RenderCustomVisual(ImDrawList* drawList, const CursorContext& ctx, CursorType type) {
        if (type != CursorType::MosaicBrush) return;

        // 渲染马赛克笔刷光标（半透明白色圆）
        const float visualRadius = GetMosaicBrushVisualRadius(ctx.mosaicBrushRadius, ctx.uiScale);
        drawList->AddCircleFilled(ctx.mousePos, visualRadius,
            GetMosaicBrushFillColor(), 0);
        drawList->AddCircle(ctx.mousePos, visualRadius,
            GetMosaicBrushOutlineColor(), 0, 1.25f);
    }

    // ─── SDL 系统光标应用 ───
    void CursorManager::ApplySDL(CursorType type) const {
        if (type == CursorType::Crosshair && m_crosshairCursor) {
            // 十字光标：覆盖 ImGui 的光标设置，使用 SDL 系统十字光标
            SDL_SetCursor(m_crosshairCursor);
        } else if (type == CursorType::Arrow) {
            // 显式恢复系统默认箭头光标，避免之前设置的十字光标残留
            //（ApplySDL 在 ImGui::Render() 之后调用，ImGui UpdateMouseCursor 不会再次运行）
            SDL_SetCursor(SDL_GetDefaultCursor());
        }
        // 其他类型（Resize/Hand 等）由 ImGui UpdateMouseCursor 正常管理，不干预
    }

    // ─── ResizeHandle → CursorType 映射 ───
    CursorType CursorManager::ResizeHandleToCursorType(ResizeHandle handle) {
        switch (handle) {
            case ResizeHandle::TopLeft:     return CursorType::ResizeNWSE;
            case ResizeHandle::BottomRight: return CursorType::ResizeNWSE;
            case ResizeHandle::TopRight:    return CursorType::ResizeNESW;
            case ResizeHandle::BottomLeft:  return CursorType::ResizeNESW;
            case ResizeHandle::Top:         return CursorType::ResizeNS;
            case ResizeHandle::Bottom:      return CursorType::ResizeNS;
            case ResizeHandle::Left:        return CursorType::ResizeEW;
            case ResizeHandle::Right:       return CursorType::ResizeEW;
            default:                        return CursorType::Arrow;
        }
    }

    // ─── CursorType → ImGuiMouseCursor 映射 ───
    // 不可 static_cast：两枚举值顺序不同（如 CursorType::ResizeNWSE=3 ≠ ImGuiMouseCursor_ResizeNWSE=6）
    ImGuiMouseCursor CursorManager::CursorTypeToImGuiMouseCursor(CursorType type) {
        switch (type) {
            case CursorType::Arrow:         return ImGuiMouseCursor_Arrow;
            case CursorType::ResizeNWSE:    return ImGuiMouseCursor_ResizeNWSE;
            case CursorType::ResizeNESW:    return ImGuiMouseCursor_ResizeNESW;
            case CursorType::ResizeNS:      return ImGuiMouseCursor_ResizeNS;
            case CursorType::ResizeEW:      return ImGuiMouseCursor_ResizeEW;
            case CursorType::ResizeAll:     return ImGuiMouseCursor_ResizeAll;
            case CursorType::Hand:          return ImGuiMouseCursor_Hand;
            case CursorType::Crosshair:     return ImGuiMouseCursor_Arrow;  // 十字光标由 SDL 实现，ImGui 侧不干预
            case CursorType::MosaicBrush:   return ImGuiMouseCursor_None;   // 马赛克光标已在 UpdateAndSetImGui 中单独设置
            default:                        return ImGuiMouseCursor_Arrow;
        }
    }

} // namespace shotlet
