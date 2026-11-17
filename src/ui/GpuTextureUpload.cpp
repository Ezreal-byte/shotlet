// 截图 GPU 纹理的创建/上传/帧内脏区合并（SDL_GPU）。
// 从 ScreenOverlayGpuTexture.cpp 提取的共享工具：ScreenOverlay 与 ImageEditor
// 共用同一套纹理上传路径（首次全量上传 + 增量脏区上传）。
#include "ui/GpuTextureUpload.h"
#include "utils/Logger.h"

#include <algorithm>
#include <cstring>

namespace shotlet {

    namespace {
        // 复用的 transfer buffer 大小上限；超过此大小的偶发大区域（如全图刷新）走临时分配
        constexpr uint32_t kMaxReusableUploadBytes = 8u * 1024u * 1024u;

        // 通过 transfer buffer 把图像 data 中 [x,y,w,h) 区域的像素（RGBA8）上传到纹理。
        // data 指向整幅图像左上角，函数内部按 (x,y) 偏移取行。
        // reusableBuffer/reusableBufferSize 非空时优先复用常驻 transfer buffer（cycle 轮转，
        // 避免写入 GPU 尚在使用的内存），尺寸不足且未超上限时按需增长。
        bool UploadPixelsToGpuTexture(SDL_GPUDevice* device, SDL_GPUTexture* texture,
                                      int x, int y, int w, int h,
                                      const unsigned char* data, int rowPitch,
                                      SDL_GPUTransferBuffer** reusableBuffer = nullptr,
                                      uint32_t* reusableBufferSize = nullptr) {
            const uint32_t uploadSize = static_cast<uint32_t>(w * h * 4);

            SDL_GPUTransferBuffer* tb = nullptr;
            bool isReusable = false; // true 时 tb 归调用方所有，本函数不释放
            if (reusableBuffer && reusableBufferSize && uploadSize <= kMaxReusableUploadBytes) {
                if (!*reusableBuffer || uploadSize > *reusableBufferSize) {
                    // 首次分配或按需增长
                    if (*reusableBuffer) {
                        SDL_ReleaseGPUTransferBuffer(device, *reusableBuffer);
                        *reusableBuffer = nullptr;
                        *reusableBufferSize = 0;
                    }
                    SDL_GPUTransferBufferCreateInfo tbInfo = {};
                    tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
                    tbInfo.size = uploadSize;
                    *reusableBuffer = SDL_CreateGPUTransferBuffer(device, &tbInfo);
                    *reusableBufferSize = *reusableBuffer ? uploadSize : 0;
                    if (!*reusableBuffer) {
                        Logger::Error("[GpuTextureUpload] SDL_CreateGPUTransferBuffer: {}", SDL_GetError());
                    }
                }
                tb = *reusableBuffer;
                isReusable = tb != nullptr;
            }

            SDL_GPUTransferBuffer* tempBuffer = nullptr;
            if (!tb) {
                // 复用不可用（超上限的大区域或分配失败）：临时分配
                SDL_GPUTransferBufferCreateInfo tbInfo = {};
                tbInfo.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
                tbInfo.size = uploadSize;
                tempBuffer = SDL_CreateGPUTransferBuffer(device, &tbInfo);
                if (!tempBuffer) {
                    Logger::Error("[GpuTextureUpload] SDL_CreateGPUTransferBuffer: {}", SDL_GetError());
                    return false;
                }
                tb = tempBuffer;
            }

            // cycle=true：复用缓冲若仍在 GPU 飞行中，SDL 内部轮转到空闲实例
            void* mapped = SDL_MapGPUTransferBuffer(device, tb, isReusable);
            if (!mapped) {
                if (tempBuffer) {
                    SDL_ReleaseGPUTransferBuffer(device, tempBuffer);
                }
                return false;
            }

            uint8_t* dst = static_cast<uint8_t*>(mapped);
            const size_t uploadRowBytes = static_cast<size_t>(w) * 4;
            for (int row = 0; row < h; row++) {
                const unsigned char* srcRow = data +
                    static_cast<size_t>(y + row) * static_cast<size_t>(rowPitch) +
                    static_cast<size_t>(x) * 4;
                memcpy(dst + static_cast<size_t>(row) * uploadRowBytes, srcRow, uploadRowBytes);
            }
            SDL_UnmapGPUTransferBuffer(device, tb);

            SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(device);
            if (!cmd) {
                if (tempBuffer) {
                    SDL_ReleaseGPUTransferBuffer(device, tempBuffer);
                }
                return false;
            }
            SDL_GPUCopyPass* copyPass = SDL_BeginGPUCopyPass(cmd);
            if (!copyPass) {
                SDL_CancelGPUCommandBuffer(cmd);
                if (tempBuffer) {
                    SDL_ReleaseGPUTransferBuffer(device, tempBuffer);
                }
                return false;
            }
            SDL_GPUTextureTransferInfo transferInfo = {};
            transferInfo.transfer_buffer = tb;
            transferInfo.pixels_per_row = static_cast<uint32_t>(w);
            SDL_GPUTextureRegion texRegion = {};
            texRegion.texture = texture;
            texRegion.x = static_cast<uint32_t>(x);
            texRegion.y = static_cast<uint32_t>(y);
            texRegion.w = static_cast<uint32_t>(w);
            texRegion.h = static_cast<uint32_t>(h);
            texRegion.d = 1;
            SDL_UploadToGPUTexture(copyPass, &transferInfo, &texRegion, false);
            SDL_EndGPUCopyPass(copyPass);
            SDL_SubmitGPUCommandBuffer(cmd);
            if (tempBuffer) {
                SDL_ReleaseGPUTransferBuffer(device, tempBuffer);
            }
            return true;
        }
    } // namespace

    SDL_GPUTexture* CreateScreenshotTexture(SDL_GPUDevice* device, int pixelWidth, int pixelHeight,
                                             const unsigned char* data, int rowPitch) {
        SDL_GPUTextureCreateInfo texInfo = {};
        texInfo.type = SDL_GPU_TEXTURETYPE_2D;
        texInfo.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        texInfo.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
        texInfo.width = static_cast<uint32_t>(pixelWidth);
        texInfo.height = static_cast<uint32_t>(pixelHeight);
        texInfo.layer_count_or_depth = 1;
        texInfo.num_levels = 1;
        texInfo.sample_count = SDL_GPU_SAMPLECOUNT_1;

        SDL_GPUTexture* gpuTex = SDL_CreateGPUTexture(device, &texInfo);
        if (!gpuTex) {
            Logger::Error("[GpuTextureUpload] SDL_CreateGPUTexture: {}", SDL_GetError());
            return nullptr;
        }

        if (!UploadPixelsToGpuTexture(device, gpuTex, 0, 0, pixelWidth, pixelHeight, data, rowPitch)) {
            SDL_WaitForGPUIdle(device);
            SDL_ReleaseGPUTexture(device, gpuTex);
            return nullptr;
        }
        return gpuTex;
    }

    void DirtyRegionUploader::Merge(int x, int y, int w, int h) {
        if (w <= 0 || h <= 0) return;
        if (!m_valid) {
            m_x = x;
            m_y = y;
            m_w = w;
            m_h = h;
            m_valid = true;
            return;
        }
        // 合并为包围矩形：同帧多次事件（马赛克涂抹可达 125-250 事件/秒）只需一次上传
        const int x1 = std::min(m_x, x);
        const int y1 = std::min(m_y, y);
        const int x2 = std::max(m_x + m_w, x + w);
        const int y2 = std::max(m_y + m_h, y + h);
        m_x = x1;
        m_y = y1;
        m_w = x2 - x1;
        m_h = y2 - y1;
    }

    void DirtyRegionUploader::Flush(SDL_GPUDevice* device, SDL_GPUTexture* texture,
                                    const unsigned char* data, int rowPitch) {
        if (!m_valid) return;
        m_valid = false;
        if (!device || !texture || !data) return;

        UploadPixelsToGpuTexture(device, texture, m_x, m_y, m_w, m_h, data, rowPitch,
                                 &m_buffer, &m_bufferSize);
    }

    void DirtyRegionUploader::Reset() {
        m_valid = false;
    }

    void DirtyRegionUploader::ReleaseBuffer(SDL_GPUDevice* device) {
        if (!m_buffer) return;
        if (device) {
            SDL_WaitForGPUIdle(device);
            SDL_ReleaseGPUTransferBuffer(device, m_buffer);
        }
        m_buffer = nullptr;
        m_bufferSize = 0;
    }

} // namespace shotlet
