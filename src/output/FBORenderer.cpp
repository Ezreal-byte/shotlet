#include "FBORenderer.h"
#include "core/Canvas.h"
#include "ui/ImGuiRendererFrame.h"
#include "utils/Colors.h"
#include "utils/Logger.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

namespace shotlet {

#if defined(SHOTLET_PLATFORM_LINUX)
    namespace {
        FBORenderer::RenderResult RenderSoftwareOutput(SDL_Renderer *renderer, ImDrawData *drawData, int width,
                                                       int height) {
            FBORenderer::RenderResult result;
            std::unique_ptr<SDL_Texture, decltype(&SDL_DestroyTexture)> target(
                    SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, width, height),
                    SDL_DestroyTexture);
            if (!target)
                return result;
            auto *previous = SDL_GetRenderTarget(renderer);
            if (!SDL_SetRenderTarget(renderer, target.get()))
                return result;
            struct RestoreTarget {
                SDL_Renderer *renderer;
                SDL_Texture *previous;
                ~RestoreTarget() { SDL_SetRenderTarget(renderer, previous); }
            } restore{renderer, previous};

            if (!SDL_SetRenderScale(renderer, float(width) / drawData->DisplaySize.x,
                                    float(height) / drawData->DisplaySize.y))
                return result;
            if (!SDL_SetRenderDrawColor(renderer, 0, 0, 0, 0) || !SDL_RenderClear(renderer))
                return result;
            SDL_ClearError();
            ImGui_ImplSDLRenderer3_RenderDrawData(drawData, renderer);
            if (*SDL_GetError()) {
                Logger::Error("[FBORenderer] Software draw failed: {}", SDL_GetError());
                return result;
            }
            std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> pixels(SDL_RenderReadPixels(renderer, nullptr),
                                                                               SDL_DestroySurface);
            if (!pixels)
                return result;
            std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> rgba(
                    SDL_ConvertSurface(pixels.get(), SDL_PIXELFORMAT_RGBA32), SDL_DestroySurface);
            if (!rgba || rgba->w != width || rgba->h != height)
                return result;
            const size_t rowBytes = size_t(width) * 4;
            result.data = std::make_unique_for_overwrite<unsigned char[]>(rowBytes * height);
            for (int y = 0; y < height; ++y) {
                std::memcpy(result.data.get() + size_t(y) * rowBytes,
                            static_cast<const unsigned char *>(rgba->pixels) + size_t(y) * rgba->pitch, rowBytes);
            }
            result.width = width;
            result.height = height;
            return result;
        }
    } // namespace
#endif

    FBORenderer::RenderResult FBORenderer::RenderToOffscreen(SDL_GPUDevice *gpuDevice, SDL_Window *sdlWindow,
                                                             ImGuiContext *imguiCtx, Canvas *canvas,
                                                             const ImRect &selLogical, float scaleFactor) {
        RenderResult result;
        const auto started = std::chrono::steady_clock::now();

        bool rendererReady = gpuDevice != nullptr;
#if defined(SHOTLET_PLATFORM_LINUX)
        rendererReady = rendererReady || (sdlWindow && SDL_GetRenderer(sdlWindow));
#endif
        if (!rendererReady || !sdlWindow || !imguiCtx || !canvas) {
            Logger::Error("[FBORenderer] Invalid parameters");
            return result;
        }

        if (!canvas->HasSelection()) {
            return result;
        }

        ImVec2 screenshotSize = canvas->GetScreenshotSize();
        ImVec2 displaySize = canvas->GetDisplaySize();
        float scaleX = scaleFactor;
        float scaleY = scaleFactor;
        if (displaySize.x > 0.0f && displaySize.y > 0.0f) {
            scaleX = canvas->GetPixelScale().x;
            scaleY = canvas->GetPixelScale().y;
        }

        // 选区像素尺寸（selLogical 是逻辑点，需 * scaleFactor）
        int selPixelW = static_cast<int>(std::lround((selLogical.Max.x - selLogical.Min.x) * scaleX));
        int selPixelH = static_cast<int>(std::lround((selLogical.Max.y - selLogical.Min.y) * scaleY));
        Logger::Debug("[FBORenderer] crop scale=({:.3f},{:.3f}), selection=({:.1f},{:.1f})-({:.1f},{:.1f})", scaleX,
                      scaleY, selLogical.Min.x, selLogical.Min.y, selLogical.Max.x, selLogical.Max.y);
        int fullPixelW = static_cast<int>(screenshotSize.x);
        int fullPixelH = static_cast<int>(screenshotSize.y);

        if (selPixelW <= 0 || selPixelH <= 0 || fullPixelW <= 0 || fullPixelH <= 0) {
            Logger::Error("[FBORenderer] Invalid dimensions: sel={}x{}, full={}x{}", selPixelW, selPixelH, fullPixelW,
                          fullPixelH);
            return result;
        }

        int selX = static_cast<int>(selLogical.Min.x * scaleX);
        int selY = static_cast<int>(selLogical.Min.y * scaleY);
        selX = std::max(0, std::min(selX, fullPixelW - 1));
        selY = std::max(0, std::min(selY, fullPixelH - 1));
        if (!canvas->HasFractionalSampling(selLogical)) {
            selPixelW = std::min(selPixelW, fullPixelW - selX);
            selPixelH = std::min(selPixelH, fullPixelH - selY);
        }

        // 无标注、无旋转且选区边缘落在整像素时，直接裁剪已有 CPU 像素。
        // 小数边缘仍走原渲染路径，保留其采样行为。
        // 水印不是图层（Canvas 级状态渲染），有水印时必须走 RenderClean 的 GPU 路径。
        const auto aligned = [](float value) { return std::fabs(value - std::round(value)) < 0.001f; };
        const int cropX = static_cast<int>(std::lround(selLogical.Min.x * scaleX));
        const int cropY = static_cast<int>(std::lround(selLogical.Min.y * scaleY));
        if (!canvas->HasFractionalSampling(selLogical) && canvas->GetImageQuarterTurns() == 0 &&
            canvas->GetLayerManager().IsEmpty() && canvas->GetWatermarkText().empty() &&
            canvas->GetCurrentImageData() && canvas->GetCurrentRowPitch() >= fullPixelW * 4 && cropX >= 0 &&
            cropY >= 0 && cropX + selPixelW <= fullPixelW && cropY + selPixelH <= fullPixelH &&
            aligned(selLogical.Min.x * scaleX) && aligned(selLogical.Min.y * scaleY) &&
            aligned(selLogical.Max.x * scaleX) && aligned(selLogical.Max.y * scaleY)) {
            result.width = selPixelW;
            result.height = selPixelH;
            const size_t rowBytes = size_t(selPixelW) * 4;
            result.data = std::make_unique_for_overwrite<unsigned char[]>(rowBytes * selPixelH);
            for (int y = 0; y < selPixelH; ++y) {
                std::memcpy(result.data.get() + size_t(y) * rowBytes,
                            canvas->GetCurrentImageData() + size_t(cropY + y) * canvas->GetCurrentRowPitch() +
                                    size_t(cropX) * 4,
                            rowBytes);
            }
            Logger::Info("event=capture.output_rendered path=cpu_crop width={} height={} total_ms={:.1f}", result.width,
                         result.height,
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
            return result;
        }

        // 旋转与预览共用坐标变换，直接在 GPU 上旋转截图和标注，保留原始像素精度。
        const ImVec2 sourceSelectionSize(selLogical.GetWidth(), selLogical.GetHeight());
        const ImageEditorTransform outputTransform{ImVec2(0, 0), 1.0f, canvas->GetImageQuarterTurns(),
                                                   sourceSelectionSize};
        const ImVec2 selectionSize = outputTransform.RotatedSize();
        if (outputTransform.quarterTurns % 2)
            std::swap(selPixelW, selPixelH);

        ImGuiContext *prevCtx = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(imguiCtx);

        // 1. 创建离屏 GPU 纹理（选区尺寸：导出/确认/OCR 时的峰值内存从
        // 全屏一份降到选区一份，5K 屏省 ~56MB；坐标系通过 renderOffset 平移）
        SDL_GPUTextureCreateInfo texInfo = {};
        texInfo.type = SDL_GPU_TEXTURETYPE_2D;
        texInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        texInfo.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        texInfo.width = static_cast<uint32_t>(selPixelW);
        texInfo.height = static_cast<uint32_t>(selPixelH);
        texInfo.layer_count_or_depth = 1;
        texInfo.num_levels = 1;
        texInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;

        SDL_GPUTexture *offscreenTex = gpuDevice ? SDL_CreateGPUTexture(gpuDevice, &texInfo) : nullptr;
        if (gpuDevice && !offscreenTex) {
            Logger::Error("[FBORenderer] SDL_CreateGPUTexture: {}", SDL_GetError());
            ImGui::SetCurrentContext(prevCtx);
            return result;
        }

        // 2. ImGui 渲染 Canvas::RenderClean 到离屏纹理
        //    窗口与 drawData 只覆盖选区，选区内容平移到原点（renderOffset）
        NewImGuiRendererFrame(gpuDevice);
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(selectionSize);
        ImGui::SetNextWindowBgAlpha(0.0f);

        ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                                 // 窗口是选区大小时，默认的窗口边框（灰色圆角矩形）会画进离屏纹理，
                                 // 导出图像边缘出现边框线；全屏窗口时代边框在图像外无此问题
                                 ImGuiWindowFlags_NoBackground;

        if (ImGui::Begin("FBOCleanRender", nullptr, flags)) {
            ImDrawList *drawList = ImGui::GetWindowDrawList();
            // Begin 默认把窗口内容裁剪到 InnerClipRect：窗口矩形四边各内缩
            // ~1-2 逻辑像素（边框宽度 + 0.5px 取整，imgui.cpp 8431-8434）。
            // 全屏窗口时代内缩在图像外无影响；选区尺寸窗口下，backend 按该
            // 裁剪矩形设置 scissor，导出图像四边会留下未绘制的透明带。
            // 离屏渲染没有边框/滚动条需要避让，裁剪矩形改回整个窗口矩形。
            // 注意 ImDrawList::PushClipRect 会对坐标向下取整：选区尺寸带小数时
            // （如 250.6）会被截为 250.0，右/下边缘仍少 1px；max 外扩 1px 逻辑
            // 单位，backend 会把超出的 scissor 钳制回纹理边界，无副作用。
            drawList->PushClipRect(ImVec2(-1.0f, -1.0f),
                                   ImVec2(sourceSelectionSize.x + 1.0f, sourceSelectionSize.y + 1.0f), false);
            drawList->AddDrawCmd();
            const int firstCommand = drawList->CmdBuffer.Size - 1;
            const int firstVertex = drawList->VtxBuffer.Size;
            canvas->RenderClean(drawList, ImVec2(-selLogical.Min.x, -selLogical.Min.y), &selLogical);
            for (int i = firstVertex; i < drawList->VtxBuffer.Size; ++i)
                drawList->VtxBuffer[i].pos = outputTransform.ToScreen(drawList->VtxBuffer[i].pos);
            for (int i = firstCommand; i < drawList->CmdBuffer.Size; ++i) {
                auto &command = drawList->CmdBuffer[i];
                const ImRect clip = outputTransform.ToScreen(ImRect(command.ClipRect));
                command.ClipRect = ImVec4(clip.Min.x, clip.Min.y, clip.Max.x, clip.Max.y);
            }
            drawList->PopClipRect();
        }
        ImGui::End();

        ImGui::Render();
        ImDrawData *drawData = ImGui::GetDrawData();
        // 投影坐标系与选区尺寸的离屏纹理对齐（io.DisplaySize 是 overlay 全屏逻辑尺寸，
        // 不改会导致内容只落在纹理左下角）。DisplaySize 用选区逻辑尺寸，
        // FramebufferScale = 纹理物理像素/逻辑点（Retina=2）。
        // backend 用 (int)(DisplaySize × FramebufferScale) 计算 viewport：浮点除法再乘回
        // 会得到 selPixelW-ε，截断后 viewport 比纹理小 1px，右/下边缘露出 clear 色白线。
        // 给分子加 0.5px 向上微扰，保证乘积截断后恰好等于纹理尺寸。
        drawData->DisplaySize = selectionSize;
        drawData->FramebufferScale =
                ImVec2(selectionSize.x > 0.0f ? (static_cast<float>(selPixelW) + 0.5f) / selectionSize.x : 1.0f,
                       selectionSize.y > 0.0f ? (static_cast<float>(selPixelH) + 0.5f) / selectionSize.y : 1.0f);

#if defined(SHOTLET_PLATFORM_LINUX)
        if (!gpuDevice) {
            // SDL_Renderer scales geometry explicitly; the GPU viewport rounding bias is unnecessary here.
            drawData->FramebufferScale = ImVec2(float(selPixelW) / selectionSize.x, float(selPixelH) / selectionSize.y);
            result = RenderSoftwareOutput(SDL_GetRenderer(sdlWindow), drawData, selPixelW, selPixelH);
            if (!result.data)
                Logger::Error("[FBORenderer] Software output failed: {}", SDL_GetError());
            Logger::Info("event=capture.output_rendered path=software width={} height={} total_ms={:.1f}", result.width,
                         result.height,
                         std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
            ImGui::SetCurrentContext(prevCtx);
            return result;
        }
#endif

        // 3. SDL_GPU 渲染到离屏纹理
        SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(gpuDevice);
        if (!cmd) {
            Logger::Error("[FBORenderer] SDL_AcquireGPUCommandBuffer: {}", SDL_GetError());
            SDL_ReleaseGPUTexture(gpuDevice, offscreenTex);
            ImGui::SetCurrentContext(prevCtx);
            return result;
        }
        ImGui_ImplSDLGPU3_PrepareDrawData(drawData, cmd);

        SDL_GPUColorTargetInfo targetInfo = {};
        targetInfo.texture = offscreenTex;
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

        // 4. 下载整个离屏纹理（纹理本身就是选区尺寸）
        uint32_t downloadSize = static_cast<uint32_t>(selPixelW * selPixelH * 4);
        SDL_GPUTransferBufferCreateInfo tbInfo = {};
        tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
        tbInfo.size = downloadSize;
        SDL_GPUTransferBuffer *downloadBuf = SDL_CreateGPUTransferBuffer(gpuDevice, &tbInfo);
        if (!downloadBuf) {
            Logger::Error("[FBORenderer] SDL_CreateGPUTransferBuffer: {}", SDL_GetError());
            SDL_SubmitGPUCommandBuffer(cmd);
            SDL_ReleaseGPUTexture(gpuDevice, offscreenTex);
            ImGui::SetCurrentContext(prevCtx);
            return result;
        }

        SDL_GPUCopyPass *copyPass = SDL_BeginGPUCopyPass(cmd);
        if (copyPass) {
            SDL_GPUTextureTransferInfo transferInfo = {};
            transferInfo.transfer_buffer = downloadBuf;
            transferInfo.offset = 0;
            SDL_GPUTextureRegion texRegion = {};
            texRegion.texture = offscreenTex;
            texRegion.x = 0;
            texRegion.y = 0;
            texRegion.w = static_cast<uint32_t>(selPixelW);
            texRegion.h = static_cast<uint32_t>(selPixelH);
            texRegion.d = 1;
            // SDL_DownloadFromGPUTexture 返回 void，无错误信号；
            // 失败会在后续 map/数据校验阶段暴露
            SDL_DownloadFromGPUTexture(copyPass, &texRegion, &transferInfo);
            SDL_EndGPUCopyPass(copyPass);
        } else {
            Logger::Error("[FBORenderer] SDL_BeginGPUCopyPass: {}", SDL_GetError());
            SDL_CancelGPUCommandBuffer(cmd);
            SDL_ReleaseGPUTransferBuffer(gpuDevice, downloadBuf);
            SDL_ReleaseGPUTexture(gpuDevice, offscreenTex);
            ImGui::SetCurrentContext(prevCtx);
            return result;
        }

        const auto submitted = std::chrono::steady_clock::now();
        SDL_GPUFence *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
        const bool completed = fence && SDL_WaitForGPUFences(gpuDevice, true, &fence, 1);
        if (fence)
            SDL_ReleaseGPUFence(gpuDevice, fence);
        if (!completed) {
            Logger::Error("[FBORenderer] GPU output submission/wait failed: {}", SDL_GetError());
            SDL_ReleaseGPUTransferBuffer(gpuDevice, downloadBuf);
            SDL_ReleaseGPUTexture(gpuDevice, offscreenTex);
            ImGui::SetCurrentContext(prevCtx);
            return result;
        }
        const auto downloaded = std::chrono::steady_clock::now();

        // 5. 读取选区像素（download region 已是紧凑的选区缓冲区）
        result.width = selPixelW;
        result.height = selPixelH;
        int rowBytes = selPixelW * 4;
        result.data = std::make_unique_for_overwrite<unsigned char[]>(size_t(rowBytes) * selPixelH);

        void *mapped = SDL_MapGPUTransferBuffer(gpuDevice, downloadBuf, false);
        if (mapped) {
            memcpy(result.data.get(), mapped, static_cast<size_t>(rowBytes) * selPixelH);
            SDL_UnmapGPUTransferBuffer(gpuDevice, downloadBuf);
        } else {
            // map 失败：result.data 是未初始化内存，绝不能返回垃圾数据当截图，
            // 置空结果让调用方走失败路径
            Logger::Error("[FBORenderer] SDL_MapGPUTransferBuffer: {}", SDL_GetError());
            result.data.reset();
            result.width = 0;
            result.height = 0;
        }

        // 6. 清理
        SDL_ReleaseGPUTransferBuffer(gpuDevice, downloadBuf);
        SDL_ReleaseGPUTexture(gpuDevice, offscreenTex);

        ImGui::SetCurrentContext(prevCtx);
        Logger::Info("event=capture.output_rendered path=gpu width={} height={} prepare_ms={:.1f} wait_ms={:.1f} "
                     "total_ms={:.1f}",
                     result.width, result.height,
                     std::chrono::duration<double, std::milli>(submitted - started).count(),
                     std::chrono::duration<double, std::milli>(downloaded - submitted).count(),
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
        return result;
    }

} // namespace shotlet
