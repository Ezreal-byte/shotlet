#pragma once
#include "WindowUtils.h"
#include <imgui_impl_sdlgpu3.h>

namespace shotlet::ImGuiUtils {

// 调用方保持 drawData 所属的 ImGui 上下文，且只传入已完成 Render 的帧。
inline bool SubmitGpuDrawData(SDL_GPUDevice* device, SDL_Window* window, ImDrawData* drawData) {
    constexpr float MinimumFramebufferPixelExtent = 1;
    // Windows 最小化不会设置 HIDDEN；也不能用上次恢复时的窗口尺寸判断是否可绘制。
    if (!WindowUtils::IsDrawable(window) || !drawData || drawData->TotalVtxCount <= 0 ||
        drawData->DisplaySize.x * drawData->FramebufferScale.x < MinimumFramebufferPixelExtent ||
        drawData->DisplaySize.y * drawData->FramebufferScale.y < MinimumFramebufferPixelExtent)
        return false;

    auto* command = SDL_AcquireGPUCommandBuffer(device);
    if (!command) return false;
    SDL_GPUTexture* swapchain = nullptr;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(command, window, &swapchain, nullptr, nullptr) || !swapchain) {
        // 最小化或恢复期间可能暂时取不到纹理，此时尚未取得交换链，直接取消本帧。
        SDL_CancelGPUCommandBuffer(command);
        return false;
    }

    ImGui_ImplSDLGPU3_PrepareDrawData(drawData, command);
    SDL_GPUColorTargetInfo target{};
    target.texture = swapchain;
    target.load_op = SDL_GPU_LOADOP_CLEAR;
    target.store_op = SDL_GPU_STOREOP_STORE;
    constexpr Uint32 ColorTargetCount = 1;
    auto* pass = SDL_BeginGPURenderPass(command, &target, ColorTargetCount, nullptr);
    if (pass) {
        ImGui_ImplSDLGPU3_RenderDrawData(drawData, command, pass);
        SDL_EndGPURenderPass(pass);
    }
    // SDL 取得交换链后禁止取消命令；即使渲染通道创建失败，也必须提交以归还交换链。
    const bool submitted = SDL_SubmitGPUCommandBuffer(command);
    return pass && submitted;
}

} // namespace shotlet::ImGuiUtils
