// ScreenOverlay 截图 GPU 纹理管理的薄封装：
// 纹理创建/全量上传/增量脏区上传的实现提取到 ui/GpuTextureUpload.cpp 共享
// （ScreenOverlay 与 ImageEditor 共用同一套上传路径），
// 这里只保留 ScreenOverlay 成员与 Canvas 数据之间的粘合。
#include "ScreenOverlay.h"
#include "core/Canvas.h"

#include <SDL3/SDL_gpu.h>

namespace shotlet {

    bool ScreenOverlay::CreateScreenshotTexture(int pixelWidth, int pixelHeight,
                                                 const unsigned char* data, int rowPitch) {
        SDL_GPUTexture* gpuTex = shotlet::CreateScreenshotTexture(m_gpuDevice, pixelWidth, pixelHeight,
                                                                data, rowPitch);
        if (!gpuTex) return false;

        m_gpuScreenshotTexture = gpuTex;
        return true;
    }

    void ScreenOverlay::MergeDirtyRegion(int x, int y, int w, int h) {
        m_uploader.Merge(x, y, w, h);
    }

    void ScreenOverlay::FlushDirtyRegion() {
        if (!m_uploader.IsValid()) return;
        const unsigned char* data = m_canvas ? m_canvas->GetCurrentImageData() : nullptr;
        const int rowPitch = m_canvas ? m_canvas->GetCurrentRowPitch() : 0;
        m_uploader.Flush(m_gpuDevice, static_cast<SDL_GPUTexture*>(m_gpuScreenshotTexture),
                         data, rowPitch);
    }

} // namespace shotlet
