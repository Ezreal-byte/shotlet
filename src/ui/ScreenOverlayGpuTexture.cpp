// ScreenOverlay 截图 GPU 纹理管理的薄封装：
// 纹理创建/全量上传/增量脏区上传的实现提取到 ui/GpuTextureUpload.cpp 共享
// （ScreenOverlay 与 ImageEditor 共用同一套上传路径），
// 这里只保留 ScreenOverlay 成员与 Canvas 数据之间的粘合。
#include "ScreenOverlay.h"
#include "core/Canvas.h"
#include "utils/Logger.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>

namespace shotlet {

    bool ScreenOverlay::CreateScreenshotTexture(int pixelWidth, int pixelHeight, const unsigned char *data,
                                                int rowPitch) {
#if defined(SHOTLET_PLATFORM_LINUX)
        if (!m_gpuDevice) {
            auto *texture = SDL_CreateTexture(SDL_GetRenderer(m_sdlWindow), SDL_PIXELFORMAT_RGBA32,
                                              SDL_TEXTUREACCESS_STATIC, pixelWidth, pixelHeight);
            if (!texture)
                return false;
            if (!SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND) ||
                !SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR) ||
                !SDL_UpdateTexture(texture, nullptr, data, rowPitch)) {
                SDL_DestroyTexture(texture);
                return false;
            }
            m_gpuScreenshotTexture = texture;
            return true;
        }
#endif
        SDL_GPUTexture *gpuTex = shotlet::CreateScreenshotTexture(m_gpuDevice, pixelWidth, pixelHeight, data, rowPitch);
        if (!gpuTex)
            return false;

        m_gpuScreenshotTexture = gpuTex;
        return true;
    }

    void ScreenOverlay::MergeDirtyRegion(int x, int y, int w, int h) { m_uploader.Merge(x, y, w, h); }

    void ScreenOverlay::FlushDirtyRegion() {
        if (!m_uploader.IsValid())
            return;
        const unsigned char *data = m_canvas ? m_canvas->GetCurrentImageData() : nullptr;
        const int rowPitch = m_canvas ? m_canvas->GetCurrentRowPitch() : 0;
#if defined(SHOTLET_PLATFORM_LINUX)
        if (!m_gpuDevice) {
            if (data && m_gpuScreenshotTexture &&
                SDL_UpdateTexture(static_cast<SDL_Texture *>(m_gpuScreenshotTexture), nullptr, data, rowPitch)) {
                m_uploader.Reset();
            } else {
                Logger::Error("Software screenshot texture update failed: {}", SDL_GetError());
            }
            return;
        }
#endif
        m_uploader.Flush(m_gpuDevice, static_cast<SDL_GPUTexture *>(m_gpuScreenshotTexture), data, rowPitch);
    }

} // namespace shotlet
