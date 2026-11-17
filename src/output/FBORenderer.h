#pragma once
#include <memory>
#include <imgui.h>
#include <imgui_internal.h>

struct SDL_Window;
struct SDL_GPUDevice;

namespace shotlet {
class Canvas;

// 选区输出：简单裁剪直接使用 CPU 像素，其余内容离屏渲染后读回。
class FBORenderer {
public:
    struct RenderResult {
        std::shared_ptr<unsigned char[]> data;
        int width = 0;
        int height = 0;
    };

    // 整像素、无标注且无旋转时直接裁剪；其余将 RenderClean 渲染到 GPU 纹理后读回。
    static RenderResult RenderToOffscreen(
        SDL_GPUDevice* gpuDevice, SDL_Window* sdlWindow,
        ImGuiContext* imguiCtx, Canvas* canvas,
        const ImRect& selLogical, float scaleFactor);
};

} // namespace shotlet
