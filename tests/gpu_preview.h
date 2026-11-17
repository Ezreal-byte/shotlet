#pragma once
#include "history/DocumentCodec.h"
#include "utils/ImGuiUtils.h"
#include "utils/ScopeExit.h"
#include <SDL3/SDL.h>
#include <imgui_impl_sdlgpu3.h>
#include <vector>

namespace shotlet::TestUtils {
// 重绘已完成的真实 ImGui 帧到离屏纹理，验证界面无需额外申请桌面截图权限。
inline bool SaveGpuPreview(SDL_GPUDevice* device, SDL_Window* window, ImGuiContext* context,
                           const std::filesystem::path& path, std::string& error) {
    constexpr Uint32 ColorTargetCount = 1;
    constexpr int RgbaChannels = 4;
    const ImGuiUtils::ContextScope current(context);
    auto* data = ImGui::GetDrawData();
    if (!data) { error = "没有已完成的界面帧"; return false; }
    const int width = int(data->DisplaySize.x * data->FramebufferScale.x);
    const int height = int(data->DisplaySize.y * data->FramebufferScale.y);
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D; info.format = SDL_GetGPUSwapchainTextureFormat(device, window);
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET;
    info.width = width; info.height = height; info.layer_count_or_depth = 1; info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    auto* texture = SDL_CreateGPUTexture(device, &info);
    if (!texture) { error = SDL_GetError(); return false; }
    const ScopeExit releaseTexture([&] { SDL_ReleaseGPUTexture(device, texture); });
    SDL_GPUTransferBufferCreateInfo bufferInfo{};
    bufferInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD; bufferInfo.size = width * height * RgbaChannels;
    auto* buffer = SDL_CreateGPUTransferBuffer(device, &bufferInfo);
    if (!buffer) { error = SDL_GetError(); return false; }
    const ScopeExit releaseBuffer([&] { SDL_ReleaseGPUTransferBuffer(device, buffer); });
    auto* command = SDL_AcquireGPUCommandBuffer(device);
    if (!command) { error = SDL_GetError(); return false; }
    const ScopeExit cancel([&] { if (command) SDL_CancelGPUCommandBuffer(command); });
    ImGui_ImplSDLGPU3_PrepareDrawData(data, command);
    SDL_GPUColorTargetInfo target{};
    target.texture = texture; target.load_op = SDL_GPU_LOADOP_CLEAR; target.store_op = SDL_GPU_STOREOP_STORE;
    auto* pass = SDL_BeginGPURenderPass(command, &target, ColorTargetCount, nullptr);
    if (!pass) { error = SDL_GetError(); return false; }
    ImGui_ImplSDLGPU3_RenderDrawData(data, command, pass); SDL_EndGPURenderPass(pass);
    auto* copy = SDL_BeginGPUCopyPass(command);
    if (!copy) { error = SDL_GetError(); return false; }
    SDL_GPUTextureRegion source{}; source.texture = texture; source.w = width; source.h = height; source.d = 1;
    SDL_GPUTextureTransferInfo destination{};
    destination.transfer_buffer = buffer; destination.pixels_per_row = width; destination.rows_per_layer = height;
    SDL_DownloadFromGPUTexture(copy, &source, &destination); SDL_EndGPUCopyPass(copy);
    auto* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command); command = nullptr;
    if (!fence) { error = SDL_GetError(); return false; }
    const ScopeExit releaseFence([&] { SDL_ReleaseGPUFence(device, fence); });
    if (!SDL_WaitForGPUFences(device, true, &fence, 1)) {
        error = SDL_GetError(); SDL_WaitForGPUIdle(device); return false;
    }
    auto* pixels = static_cast<unsigned char*>(SDL_MapGPUTransferBuffer(device, buffer, false));
    if (!pixels) { error = SDL_GetError(); return false; }
    const ScopeExit unmap([&] { SDL_UnmapGPUTransferBuffer(device, buffer); });
    std::vector<unsigned char> rgba(pixels, pixels + size_t(width) * height * RgbaChannels);
    if (info.format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM || info.format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM_SRGB)
        for (size_t i = 0; i < rgba.size(); i += RgbaChannels) std::swap(rgba[i], rgba[i + 2]);
    return WriteHistoryPng(path, rgba.data(), width, height, error);
}
}
