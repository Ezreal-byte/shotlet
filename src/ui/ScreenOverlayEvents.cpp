// ScreenOverlay 输入事件分发：
// 负责 SDL 事件（非 Windows 平台）以及 Windows 全局鼠标状态轮询
// 到 Canvas 鼠标/键盘交互的转换，包含 Windows 专属的双击检测状态机。
#include "ScreenOverlay.h"
#include "core/Canvas.h"
#include "platform/TransparentWindow.h"
#include "ui/Toolbar.h"
#include "ui/SubToolbar.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_keycode.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>

#if defined(SHOTLET_PLATFORM_WINDOWS)
#include "platform/PlatformWin32.h"
#endif

namespace shotlet {

    void ScreenOverlay::ProcessEvent(const SDL_Event& event) {
#if defined(SHOTLET_PLATFORM_MACOS)
        if (m_scrollCapture) return;
#endif
#if defined(SHOTLET_PLATFORM_WINDOWS)
        if (!m_created || !m_imguiContext) return;

        ImGuiContext* prevCtx = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(m_imguiContext);
        ImGui_ImplSDL3_ProcessEvent(const_cast<SDL_Event*>(&event));
        // 子工具栏输入框聚焦时按键归 ImGui 处理，不再转发给画布（与 ImageEditor 一致）。
        const bool imguiWantsText = ImGui::GetIO().WantTextInput;
        ImGui::SetCurrentContext(prevCtx);

        if (event.type == SDL_EVENT_KEY_DOWN && m_canvas &&
            (m_canvas->IsTextInputActive() || !imguiWantsText)) {
            m_canvas->OnKeyDown(event.key.key, event.key.mod, event.key.repeat);
        }

        // Windows mouse input is handled from ProcessGlobalMouseState because
        // the SDL window can miss pointer messages in the layered/topmost path.
#else
        if (!m_created || !m_imguiContext) return;

#if defined(SHOTLET_PLATFORM_LINUX)
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
            event.key.key == SDLK_S && (event.key.mod & SDL_KMOD_CTRL) &&
            m_canvas && m_canvas->HasSelection()) {
            m_saveRequested = true;
            m_hasResult = true;
            return;
        }
#endif

        ImGuiContext* prevCtx = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(m_imguiContext);
        ImGui_ImplSDL3_ProcessEvent(const_cast<SDL_Event*>(&event));
        const bool imguiWantsText = ImGui::GetIO().WantTextInput;
        ImGui::SetCurrentContext(prevCtx);

        if (event.type == SDL_EVENT_KEY_DOWN && m_canvas &&
            (m_canvas->IsTextInputActive() || !imguiWantsText)) {
            m_canvas->OnKeyDown(event.key.key, event.key.mod, event.key.repeat);
        }

        if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            float x = static_cast<float>(event.button.x);
            float y = static_cast<float>(event.button.y);
            ImVec2 mousePos(x, y);
            ImGui::SetCurrentContext(m_imguiContext);
            bool hasItemHovered = ImGui::IsAnyItemHovered();
            ImGui::SetCurrentContext(prevCtx);

            bool inToolbarArea = false;
            if (m_toolbar) inToolbarArea |= m_toolbar->GetWindowRect().Contains(mousePos);
            if (m_subToolbar) inToolbarArea |= m_subToolbar->GetWindowRect().Contains(mousePos);

            if (!hasItemHovered && !inToolbarArea && m_canvas && !m_selectionLocked) {
                Uint8 clicks = event.button.clicks;
                if (clicks == 2) {
                    m_canvas->OnMouseDoubleClick(ImVec2(x, y));
                    return;
                }
                m_canvas->OnMouseDown(ImVec2(x, y));
                if (!m_nsAppActivated && m_canvas->HasSelection() && m_active) {
                    m_nsAppActivated = true;
                    SDL_StartTextInput(m_sdlWindow);
                }
            }
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT) {
            float x = static_cast<float>(event.button.x);
            float y = static_cast<float>(event.button.y);
            ImVec2 mousePos(x, y);
            ImGui::SetCurrentContext(m_imguiContext);
            bool hasItemHovered = ImGui::IsAnyItemHovered();
            ImGui::SetCurrentContext(prevCtx);

            bool inToolbarArea = false;
            if (m_toolbar) inToolbarArea |= m_toolbar->GetWindowRect().Contains(mousePos);
            if (m_subToolbar) inToolbarArea |= m_subToolbar->GetWindowRect().Contains(mousePos);

            if (!hasItemHovered && !inToolbarArea && m_canvas) {
                m_canvas->OnMouseUp(ImVec2(x, y));
            }
        } else if (event.type == SDL_EVENT_MOUSE_MOTION && (event.motion.state & SDL_BUTTON_LMASK)) {
            float x = static_cast<float>(event.motion.x);
            float y = static_cast<float>(event.motion.y);
            if (m_canvas) {
                m_canvas->OnMouseMove(ImVec2(x, y));
            }
        }
#endif
    }

#if defined(SHOTLET_PLATFORM_WINDOWS)
    void ScreenOverlay::ProcessGlobalMouseState(int globalX, int globalY, bool leftDown) {
        if (!m_created || !m_canvas || !m_imguiContext) return;

        ImGuiContext* prevCtx = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(m_imguiContext);

        const float localX = static_cast<float>(globalX - m_config.physicalX);
        const float localY = static_cast<float>(globalY - m_config.physicalY);
        const ImVec2 mousePos(localX, localY);
        const bool inside = localX >= 0.0f && localY >= 0.0f &&
                            localX < static_cast<float>(m_config.physicalWidth) &&
                            localY < static_cast<float>(m_config.physicalHeight);

        if (leftDown && !m_winLeftWasDown) {
            m_winMouseDownOnCanvas = false;
            if (inside && !m_selectionLocked) {
                bool inToolbarArea = false;
                if (m_toolbar) inToolbarArea |= m_toolbar->GetWindowRect().Contains(mousePos);
                if (m_subToolbar) inToolbarArea |= m_subToolbar->GetWindowRect().Contains(mousePos);
                if (!inToolbarArea) {
                    // 双击检测：与上次点击时间间隔 < GetDoubleClickTime() 且位置接近
                    uint32_t now = static_cast<uint32_t>(GetTickCount());
                    uint32_t doubleClickTime = static_cast<uint32_t>(GetDoubleClickTime());
                    float dx = mousePos.x - m_winLastClickPosX;
                    float dy = mousePos.y - m_winLastClickPosY;
                    bool isDoubleClick = (now - m_winLastClickTime) < doubleClickTime &&
                                         (dx * dx + dy * dy) < 36.0f;  // 6px 容差

                    if (isDoubleClick && m_canvas->GetTool() != ToolType::Number) {
                        // 完成第一次点击可能遗留的选区操作
                        if (m_winMouseDownOnCanvas) {
                            m_canvas->OnMouseUp(mousePos);
                            m_winMouseDownOnCanvas = false;
                        }
                        // 重新检测智能区域（第一次点击的 OnMouseDown 可能已启动选区，导致智能区域不再更新）
                        if (!m_canvas->HasSelection() && m_smartDetector && m_smartCacheBuilt) {
                            const SmartRegion* region = m_smartDetector->FindBestRegion(mousePos.x, mousePos.y);
                            if (region) {
                                m_canvas->SetSmartRegion(region);
                            }
                        }
                        m_canvas->OnMouseDoubleClick(mousePos);
                        // 双击已处理，重置计时避免三击误判
                        m_winLastClickTime = 0;
                    } else {
                        bool textInputWasActive = m_canvas->IsTextInputActive();
                        m_canvas->OnMouseDown(mousePos);
                        if (!textInputWasActive && m_canvas->IsTextInputActive() && m_sdlWindow) {
                            ForceWindowFocus(m_sdlWindow);
                            SDL_StartTextInput(m_sdlWindow);
                        }
                        m_winMouseDownOnCanvas = true;
                        m_winLastClickTime = now;
                        m_winLastClickPosX = mousePos.x;
                        m_winLastClickPosY = mousePos.y;
                    }
                }
            }
        } else if (leftDown && m_winLeftWasDown && m_winMouseDownOnCanvas) {
            m_canvas->OnMouseMove(mousePos);
        } else if (!leftDown && m_winLeftWasDown) {
            if (m_winMouseDownOnCanvas) {
                m_canvas->OnMouseUp(mousePos);
            }
            m_winMouseDownOnCanvas = false;
        }

        m_winLeftWasDown = leftDown;
        ImGui::SetCurrentContext(prevCtx);
    }
#endif

} // namespace shotlet
