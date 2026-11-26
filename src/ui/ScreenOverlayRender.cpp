#include "utils/Colors.h"
// ScreenOverlay 逐帧渲染：
// 负责 SmartRegion 缓存的延迟构建与更新、ImGui 帧的绘制（截图/图层/工具栏/
// 暗化蒙层）、IME 光标区域设置、光标应用，以及最终的 SDL_GPU 提交。
#include "ScreenOverlay.h"
#include "core/Canvas.h"
#include "core/CursorManager.h"
#include "ui/ImGuiRendererFrame.h"
#include "ui/SubToolbar.h"
#include "ui/Toolbar.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

namespace shotlet {

    void ScreenOverlay::RenderFrame() {
        if (!m_created || m_hidden || !m_sdlWindow || !m_imguiContext)
            return;
#if defined(SHOTLET_PLATFORM_LINUX)
        if (!m_gpuDevice && !SDL_GetRenderer(m_sdlWindow))
            return;
#else
        if (!m_gpuDevice)
            return;
#endif

#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)
        PollScrollCapture();
        // 采集界面由原生透明窗口绘制；仍需每帧轮询完成、取消和长图预览。
        if (m_scrollCapture || m_hidden)
            return;
#endif
        // 事件阶段累积的纹理脏区在此一次性上传（马赛克涂抹等高频路径）
        FlushDirtyRegion();

        ImGuiContext *prevCtx = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(m_imguiContext);

        ImGuiIO &io = ImGui::GetIO();
        bool hadNoMouseCursorChange = io.ConfigFlags & ImGuiConfigFlags_NoMouseCursorChange;
#if !defined(SHOTLET_PLATFORM_WINDOWS)
        if (!m_active) {
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
        }
#endif

        NewImGuiRendererFrame(m_gpuDevice);
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        // Smart region detection（首次使用时延迟构建缓存）
        if (m_canvas && !m_canvas->HasSelection() && !m_dimmed && m_smartDetector) {
            if (!m_smartCacheBuilt) {
                ImVec2 displaySize = m_canvas->GetDisplaySize();
                m_smartDetector->BuildCache(static_cast<float>(m_config.windowX), static_cast<float>(m_config.windowY),
                                            displaySize.x, displaySize.y);
                m_smartCacheBuilt = true;
            }
            ImVec2 mousePos = ImGui::GetIO().MousePos;
            float dx = mousePos.x - m_lastSmartDetectMousePos.x;
            float dy = mousePos.y - m_lastSmartDetectMousePos.y;
            if (dx * dx + dy * dy > 4.0f) {
                m_lastSmartDetectMousePos = mousePos;
                m_canvas->SetSmartRegion(m_smartDetector->FindBestRegion(mousePos.x, mousePos.y));
            }
        } else if (m_canvas && m_dimmed) {
            m_canvas->ClearSmartRegion();
        }
        // 注意：HasSelection() 时不清除智能区域数据，
        // 因为双击确认智能选区时仍需要该数据（第一次点击的 OnMouseDown 已启动选区）

        // Fullscreen transparent window
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        ImGui::SetNextWindowBgAlpha(0.0f);

        ImGuiWindowFlags wf = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                              ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                              ImGuiWindowFlags_NoMouseInputs;

        if (ImGui::Begin("OverlayWindow", nullptr, wf)) {
            ImDrawList *drawList = ImGui::GetWindowDrawList();
            m_canvas->Render(drawList);
            if (m_dimmed) {
                drawList->AddRectFilled(ImVec2(0, 0), ImGui::GetIO().DisplaySize, Colors::DimOverlay);
            }
        }
        ImGui::End();

#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)
        if (!m_scrollMessage.empty() && !m_scrollCapture) {
            ImGui::SetNextWindowPos(ImVec2(16, 16));
            ImGui::Begin(m_scrollPromptTitle.c_str(), nullptr,
                         ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse);
            ImGui::TextWrapped("%s", m_scrollMessage.c_str());
            if (m_pendingScrollResult) {
                if (ImGui::Button(m_scrollRetryLoadLabel.c_str()))
                    m_scrollRequested = true;
                ImGui::SameLine();
                if (ImGui::Button(m_scrollSaveLabel.c_str()))
                    m_saveScrollResult = true;
            } else if (ImGui::Button(m_scrollCloseLabel.c_str()))
                m_scrollMessage.clear();
            ImGui::End();
        }
        if (!m_scrollCapture && !m_pendingScrollResult) {
#endif
            if (m_toolbar && !m_dimmed && m_canvas->HasSelection())
                m_toolbar->Render();
            if (m_subToolbar && !m_dimmed && m_canvas->HasSelection())
                m_subToolbar->Render();

#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)
        }
#endif
        if (m_canvas && m_canvas->IsTextInputActive()) {
            if (m_sdlWindow) {
                ImVec2 imePos = m_canvas->GetIMECursorPos();
                SDL_Rect imeRect;
                imeRect.x = static_cast<int>(imePos.x);
                imeRect.y = static_cast<int>(imePos.y);
                imeRect.w = 1;
                imeRect.h = static_cast<int>(m_canvas->GetTextFontSize());
                SDL_SetTextInputArea(m_sdlWindow, &imeRect, 0);
            }
        }

        ImGui::Render();

        if (m_active && m_cursorManager && m_canvas) {
            m_cursorManager->ApplySDL(m_canvas->GetLastCursorType());
        }

        ImDrawData *drawData = ImGui::GetDrawData();
#if defined(SHOTLET_PLATFORM_LINUX)
        if (!m_gpuDevice && drawData) {
            auto *renderer = SDL_GetRenderer(m_sdlWindow);
            SDL_SetRenderScale(renderer, drawData->FramebufferScale.x, drawData->FramebufferScale.y);
            SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(drawData, renderer);
            SDL_RenderPresent(renderer);
        } else
#endif
                if (drawData && drawData->TotalVtxCount > 0) {
            SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(m_gpuDevice);
            SDL_GPUTexture *swapchainTex = nullptr;
            if (SDL_AcquireGPUSwapchainTexture(cmd, m_sdlWindow, &swapchainTex, nullptr, nullptr) && swapchainTex) {
                ImGui_ImplSDLGPU3_PrepareDrawData(drawData, cmd);

                SDL_GPUColorTargetInfo targetInfo = {};
                targetInfo.texture = swapchainTex;
                const auto clear = Colors::TransparentVec4;
                targetInfo.clear_color = SDL_FColor{clear.x, clear.y, clear.z, clear.w};
                targetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
                targetInfo.store_op = SDL_GPU_STOREOP_STORE;
                targetInfo.mip_level = 0;
                targetInfo.layer_or_depth_plane = 0;
                targetInfo.cycle = false;

                SDL_GPURenderPass *renderPass = SDL_BeginGPURenderPass(cmd, &targetInfo, 1, nullptr);
                ImGui_ImplSDLGPU3_RenderDrawData(drawData, cmd, renderPass);
                SDL_EndGPURenderPass(renderPass);
            }

            SDL_SubmitGPUCommandBuffer(cmd);
        }

#if !defined(SHOTLET_PLATFORM_WINDOWS)
        if (!m_active && !hadNoMouseCursorChange) {
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        }
#endif

        ImGui::SetCurrentContext(prevCtx);
    }

} // namespace shotlet
