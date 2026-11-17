#pragma once
#include <SDL3/SDL_gpu.h>

namespace shotlet {

// 创建截图 GPU 纹理并完成首次全量上传（RGBA8 + SAMPLER）；失败返回 nullptr
SDL_GPUTexture* CreateScreenshotTexture(SDL_GPUDevice* device, int pixelWidth, int pixelHeight,
                                        const unsigned char* data, int rowPitch);

// 截图纹理的帧内脏区合并 + 增量上传（马赛克涂抹/撤销后刷新路径）。
// 事件阶段 Merge 只累积脏区矩形，RenderFrame 开头 Flush 一次性上传，
// 避免高频鼠标事件每次都创建/提交 GPU command buffer；
// 常驻 transfer buffer 复用（cycle 轮转避免写入 GPU 尚在使用的内存）。
struct DirtyRegionUploader {
    // 累积脏区（合并为包围矩形，同帧多次事件只需一次上传）
    void Merge(int x, int y, int w, int h);
    // 上传当前脏区；纹理与像素数据由调用方提供，无效参数时丢弃脏区
    void Flush(SDL_GPUDevice* device, SDL_GPUTexture* texture,
               const unsigned char* data, int rowPitch);
    // 丢弃未上传的脏区（截图内容被整体替换时）
    void Reset();
    // 释放常驻 transfer buffer（销毁/复用前调用；device 为空时仅清引用）
    void ReleaseBuffer(SDL_GPUDevice* device);

    bool IsValid() const { return m_valid; }

private:
    bool m_valid = false;
    int m_x = 0, m_y = 0, m_w = 0, m_h = 0;
    SDL_GPUTransferBuffer* m_buffer = nullptr;
    uint32_t m_bufferSize = 0;
};

} // namespace shotlet
