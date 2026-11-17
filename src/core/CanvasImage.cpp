// Canvas 图像与纹理处理逻辑：
// 负责马赛克预计算、遮罩合成、图层回写到图像数据、
// 以及 GPU 纹理更新与图像副本导出。
#include "Canvas.h"
#include "utils/Logger.h"
#include <algorithm>
#include <cfloat>

namespace shotlet {
    void Canvas::SetMosaicBrushIndex(const int index) {
        m_mosaicBrushIndex = index;
        m_mosaicBrushRadius = MOSAIC_BRUSH_OPTIONS[index];
        // 已提交的 MosaicLayer 需要重建 mask 并 composite
        if (!m_layerManager.IsEmpty()) {
            ConditionalRefreshTexture();
        }
    }

    ImU32 Canvas::DarkenColor(const ImU32 color, const float factor) {
        const int r = static_cast<int>(((color >> 0) & 0xFF) * factor);
        const int g = static_cast<int>(((color >> 8) & 0xFF) * factor);
        const int b = static_cast<int>(((color >> 16) & 0xFF) * factor);
        const int a = static_cast<int>(((color >> 24) & 0xFF));
        return IM_COL32(r, g, b, a);
    }

    void Canvas::EnsureEditRegion(const ImRect& regionPx) {
        if (!m_imageData) return;

        const int imgWidth = static_cast<int>(m_screenshotSize.x);
        const int imgHeight = static_cast<int>(m_screenshotSize.y);

        // 夹到图像范围
        const int minX = std::max(0, static_cast<int>(std::floor(regionPx.Min.x)));
        const int minY = std::max(0, static_cast<int>(std::floor(regionPx.Min.y)));
        const int maxX = std::min(imgWidth, static_cast<int>(std::ceil(regionPx.Max.x)));
        const int maxY = std::min(imgHeight, static_cast<int>(std::ceil(regionPx.Max.y)));
        if (minX >= maxX || minY >= maxY) return;

        const bool hadMosaic = m_mosaicImageData != nullptr;

        if (m_editW == 0) {
            // 首次建立：original 从 imageData 拷贝（此时区域内 == 原始像素）
            m_editX = minX;
            m_editY = minY;
            m_editW = maxX - minX;
            m_editH = maxY - minY;
            m_originalImageData = std::make_unique<unsigned char[]>(
                static_cast<size_t>(m_editW) * m_editH * 4);
            for (int y = 0; y < m_editH; y++) {
                memcpy(m_originalImageData.get() + static_cast<size_t>(y) * m_editW * 4,
                       m_imageData.get() + static_cast<size_t>(y + m_editY) * m_imageRowPitch +
                           static_cast<size_t>(m_editX) * 4,
                       static_cast<size_t>(m_editW) * 4);
            }
            return;
        }

        // 已覆盖则无需扩展
        if (minX >= m_editX && minY >= m_editY &&
            maxX <= m_editX + m_editW && maxY <= m_editY + m_editH) {
            return;
        }

        // 扩展 = union。编辑只发生在旧区域内，新增区域在 m_imageData 中仍是原始像素：
        // 新缓冲整块从 imageData 拷贝，再把旧 original 重叠区覆盖回去
        const int newX = std::min(minX, m_editX);
        const int newY = std::min(minY, m_editY);
        const int newW = std::max(maxX, m_editX + m_editW) - newX;
        const int newH = std::max(maxY, m_editY + m_editH) - newY;

        auto newOriginal = std::make_unique<unsigned char[]>(
            static_cast<size_t>(newW) * newH * 4);
        for (int y = 0; y < newH; y++) {
            memcpy(newOriginal.get() + static_cast<size_t>(y) * newW * 4,
                   m_imageData.get() + static_cast<size_t>(y + newY) * m_imageRowPitch +
                       static_cast<size_t>(newX) * 4,
                   static_cast<size_t>(newW) * 4);
        }
        for (int y = 0; y < m_editH; y++) {
            memcpy(newOriginal.get() + static_cast<size_t>(y + m_editY - newY) * newW * 4 +
                       static_cast<size_t>(m_editX - newX) * 4,
                   m_originalImageData.get() + static_cast<size_t>(y) * m_editW * 4,
                   static_cast<size_t>(m_editW) * 4);
        }
        m_originalImageData = std::move(newOriginal);

        m_editX = newX;
        m_editY = newY;
        m_editW = newW;
        m_editH = newH;

        // mosaic/mask 缓冲随区域重建（mask 会重涂全部笔迹）
        m_mosaicImageData.reset();
        m_maskData.reset();
        if (hadMosaic) {
            EnsureMosaicBuffers();
        }
    }

    void Canvas::EnsureMosaicBuffers() {
        if (m_editW == 0 || !m_originalImageData) return;
        if (m_mosaicImageData && m_maskData) return;

        const int blockSize = m_mosaicBlockSize;
        const size_t pitch = static_cast<size_t>(m_editW) * 4;

        m_mosaicImageData = std::make_unique<unsigned char[]>(pitch * m_editH);
        m_maskData = std::make_unique<unsigned char[]>(static_cast<size_t>(m_editW) * m_editH);
        memset(m_maskData.get(), 0, static_cast<size_t>(m_editW) * m_editH);

        // 区域马赛克：块网格按全局像素坐标对齐（与全图算法同一网格），
        // 块与编辑区域相交的部分取区域内像素平均
        for (int by = ((m_editY + m_mosaicGridOffsetY) / blockSize) * blockSize - m_mosaicGridOffsetY;
             by < m_editY + m_editH; by += blockSize) {
            for (int bx = ((m_editX + m_mosaicGridOffsetX) / blockSize) * blockSize - m_mosaicGridOffsetX;
                 bx < m_editX + m_editW; bx += blockSize) {
                const int x0 = std::max(bx, m_editX);
                const int x1 = std::min(bx + blockSize, m_editX + m_editW);
                const int y0 = std::max(by, m_editY);
                const int y1 = std::min(by + blockSize, m_editY + m_editH);
                if (x0 >= x1 || y0 >= y1) continue;

                int64_t r = 0, g = 0, b = 0, a = 0;
                int count = 0;
                for (int y = y0; y < y1; y++) {
                    const unsigned char* srcRow = m_originalImageData.get() +
                        static_cast<size_t>(y - m_editY) * pitch;
                    for (int x = x0; x < x1; x++) {
                        const int idx = (x - m_editX) * 4;
                        r += srcRow[idx];
                        g += srcRow[idx + 1];
                        b += srcRow[idx + 2];
                        a += srcRow[idx + 3];
                        count++;
                    }
                }
                if (count > 0) {
                    r /= count; g /= count; b /= count; a /= count;
                }
                for (int y = y0; y < y1; y++) {
                    unsigned char* dstRow = m_mosaicImageData.get() +
                        static_cast<size_t>(y - m_editY) * pitch;
                    for (int x = x0; x < x1; x++) {
                        const int idx = (x - m_editX) * 4;
                        dstRow[idx]     = static_cast<unsigned char>(r);
                        dstRow[idx + 1] = static_cast<unsigned char>(g);
                        dstRow[idx + 2] = static_cast<unsigned char>(b);
                        dstRow[idx + 3] = static_cast<unsigned char>(a);
                    }
                }
            }
        }

        // 重涂已提交笔迹（缓冲新建/区域扩展后 mask 需要重建）
        RebuildMaskRegion(ImRect(ImVec2(static_cast<float>(m_editX), static_cast<float>(m_editY)),
                                 ImVec2(static_cast<float>(m_editX + m_editW),
                                        static_cast<float>(m_editY + m_editH))));
    }

    void Canvas::PaintMosaicMask(const ImVec2& point, const float brushRadius) {
        if (!m_maskData || m_editW == 0) return;

        const float scaleX = GetPixelScale().x;
        const float scaleY = GetPixelScale().y;
        const float px = ToPixel(point).x;
        const float py = ToPixel(point).y;
        const float radiusPx = brushRadius * scaleX;

        // 径向渐变圆的范围（mask 局部坐标）
        const int minX = std::max(0, static_cast<int>(px - radiusPx) - m_editX);
        const int maxX = std::min(m_editW - 1, static_cast<int>(px + radiusPx) - m_editX);
        const int minY = std::max(0, static_cast<int>(py - radiusPx) - m_editY);
        const int maxY = std::min(m_editH - 1, static_cast<int>(py + radiusPx) - m_editY);
        if (minX > maxX || minY > maxY) return;

        const float innerRadius = radiusPx * 0.7f;  // 渐变内半径
        // 距离平方预判：圆外直接跳过，sqrt 只在渐变带内计算
        const float radiusSq = radiusPx * radiusPx;
        const float innerRadiusSq = innerRadius * innerRadius;

        for (int y = minY; y <= maxY; y++) {
            for (int x = minX; x <= maxX; x++) {
                // 距离在全局像素坐标系中计算
                const float dx = static_cast<float>(x + m_editX) - px;
                const float dy = static_cast<float>(y + m_editY) - py;
                const float distSq = dx * dx + dy * dy;
                if (distSq > radiusSq) continue;

                // smoothstep 渐变：innerRadius 内 alpha=1.0，innerRadius 到 radiusPx 从 1→0
                float alpha;
                if (distSq <= innerRadiusSq) {
                    alpha = 1.0f;
                } else {
                    const float dist = std::sqrt(distSq);
                    const float t = (dist - innerRadius) / (radiusPx - innerRadius);
                    alpha = 1.0f - t * t * (3.0f - 2.0f * t);  // smoothstep 反向
                }

                // lighter 合成：叠加，clamp 到 255
                const int maskIdx = y * m_editW + x;
                int newVal = m_maskData[maskIdx] + static_cast<int>(alpha * 255.0f);
                m_maskData[maskIdx] = static_cast<unsigned char>(std::min(newVal, 255));
            }
        }
    }

    void Canvas::CompositeMosaicRegion(const ImRect& regionPx) {
        if (!m_imageData || !m_originalImageData || m_editW == 0) return;

        // region 与编辑区域求交（全局像素坐标），访问时换算为缓冲局部坐标
        const int gMinX = std::max(m_editX, static_cast<int>(regionPx.Min.x));
        const int gMaxX = std::min(m_editX + m_editW - 1, static_cast<int>(regionPx.Max.x));
        const int gMinY = std::max(m_editY, static_cast<int>(regionPx.Min.y));
        const int gMaxY = std::min(m_editY + m_editH - 1, static_cast<int>(regionPx.Max.y));
        if (gMinX > gMaxX || gMinY > gMaxY) return;

        const size_t editPitch = static_cast<size_t>(m_editW) * 4;
        // 整数先比较 mask 档位（0/255 的快路径），浮点除法换成乘常数
        constexpr float kInv255 = 1.0f / 255.0f;
        for (int gy = gMinY; gy <= gMaxY; gy++) {
            const int ly = gy - m_editY;
            for (int gx = gMinX; gx <= gMaxX; gx++) {
                const int lx = gx - m_editX;
                const int maskIdx = ly * m_editW + lx;
                const int maskVal = m_maskData ? m_maskData[maskIdx] : 0;
                const int imgIdx = gy * m_imageRowPitch + gx * 4;
                const int origIdx = static_cast<int>(ly * editPitch) + lx * 4;
                if (maskVal == 0) {
                    // 完全原图，直接拷贝原始数据
                    m_imageData[imgIdx]     = m_originalImageData[origIdx];
                    m_imageData[imgIdx + 1] = m_originalImageData[origIdx + 1];
                    m_imageData[imgIdx + 2] = m_originalImageData[origIdx + 2];
                    m_imageData[imgIdx + 3] = m_originalImageData[origIdx + 3];
                } else if (maskVal >= 255) {
                    // 完全马赛克（mosaic 未分配时 mask 不可能有值，防御性回退原图）
                    const int mosIdx = origIdx;
                    if (m_mosaicImageData) {
                        m_imageData[imgIdx]     = m_mosaicImageData[mosIdx];
                        m_imageData[imgIdx + 1] = m_mosaicImageData[mosIdx + 1];
                        m_imageData[imgIdx + 2] = m_mosaicImageData[mosIdx + 2];
                        m_imageData[imgIdx + 3] = m_mosaicImageData[mosIdx + 3];
                    } else {
                        m_imageData[imgIdx]     = m_originalImageData[origIdx];
                        m_imageData[imgIdx + 1] = m_originalImageData[origIdx + 1];
                        m_imageData[imgIdx + 2] = m_originalImageData[origIdx + 2];
                        m_imageData[imgIdx + 3] = m_originalImageData[origIdx + 3];
                    }
                } else {
                    // 混合：lerp(original, mosaic, mask)
                    const float maskAlpha = static_cast<float>(maskVal) * kInv255;
                    const float inv = 1.0f - maskAlpha;
                    m_imageData[imgIdx]     = static_cast<unsigned char>(m_originalImageData[origIdx]     * inv + m_mosaicImageData[origIdx]     * maskAlpha);
                    m_imageData[imgIdx + 1] = static_cast<unsigned char>(m_originalImageData[origIdx + 1] * inv + m_mosaicImageData[origIdx + 1] * maskAlpha);
                    m_imageData[imgIdx + 2] = static_cast<unsigned char>(m_originalImageData[origIdx + 2] * inv + m_mosaicImageData[origIdx + 2] * maskAlpha);
                    m_imageData[imgIdx + 3] = 255;
                }
            }
        }
    }

    ImRect Canvas::ComputeRefreshRegionPx() const {
        const int imgWidth = static_cast<int>(m_screenshotSize.x);
        const int imgHeight = static_cast<int>(m_screenshotSize.y);

        // 选区（逻辑坐标）→ 像素坐标；未选区时保守取全图
        ImRect regionPx;
        if (HasSelection()) {
            const ImRect sel = GetSelectionRect();
            float scaleX = 1.0f, scaleY = 1.0f;
            if (m_displaySize.x > 0 && m_displaySize.y > 0) {
                scaleX = GetPixelScale().x;
                scaleY = GetPixelScale().y;
            }
            // 外扩最大画笔半径：涂抹点被夹在选区内，但笔刷圆可越出边界
            const float maxBrushPx = MOSAIC_BRUSH_OPTIONS[2] * std::max(scaleX, scaleY);
            regionPx = ImRect(
                ImVec2(ToPixel(sel.Min).x - maxBrushPx, ToPixel(sel.Min).y - maxBrushPx),
                ImVec2(ToPixel(sel.Max).x + maxBrushPx, ToPixel(sel.Max).y + maxBrushPx));
        } else {
            regionPx = ImRect(ImVec2(0.0f, 0.0f),
                              ImVec2(static_cast<float>(imgWidth), static_cast<float>(imgHeight)));
        }

        // 夹到图像范围
        regionPx.Min.x = std::max(0.0f, regionPx.Min.x);
        regionPx.Min.y = std::max(0.0f, regionPx.Min.y);
        regionPx.Max.x = std::min(static_cast<float>(imgWidth), regionPx.Max.x);
        regionPx.Max.y = std::min(static_cast<float>(imgHeight), regionPx.Max.y);
        return regionPx;
    }

    void Canvas::RestoreRegionFromOriginal(const ImRect& regionPx) {
        if (!m_imageData || !m_originalImageData || m_editW == 0) return;

        // region 与编辑区域求交（全局像素坐标）
        const int gMinX = std::max(m_editX, static_cast<int>(regionPx.Min.x));
        const int gMinY = std::max(m_editY, static_cast<int>(regionPx.Min.y));
        const int gMaxX = std::min(m_editX + m_editW, static_cast<int>(std::ceil(regionPx.Max.x)));
        const int gMaxY = std::min(m_editY + m_editH, static_cast<int>(std::ceil(regionPx.Max.y)));
        if (gMinX >= gMaxX || gMinY >= gMaxY) return;

        const int rowBytes = (gMaxX - gMinX) * 4;
        const size_t editPitch = static_cast<size_t>(m_editW) * 4;
        for (int gy = gMinY; gy < gMaxY; gy++) {
            memcpy(m_imageData.get() + static_cast<size_t>(gy) * m_imageRowPitch + static_cast<size_t>(gMinX) * 4,
                   m_originalImageData.get() + static_cast<size_t>(gy - m_editY) * editPitch +
                       static_cast<size_t>(gMinX - m_editX) * 4,
                   static_cast<size_t>(rowBytes));
        }
    }

    void Canvas::RebuildMaskRegion(const ImRect& regionPx) {
        if (!m_maskData || m_editW == 0) return;

        // 清空区域 mask（region 与编辑区域求交后换算为局部坐标）
        const int lMinX = std::max(0, static_cast<int>(regionPx.Min.x) - m_editX);
        const int lMinY = std::max(0, static_cast<int>(regionPx.Min.y) - m_editY);
        const int lMaxX = std::min(m_editW, static_cast<int>(std::ceil(regionPx.Max.x)) - m_editX);
        const int lMaxY = std::min(m_editH, static_cast<int>(std::ceil(regionPx.Max.y)) - m_editY);
        if (lMinX >= lMaxX || lMinY >= lMaxY) return;

        for (int y = lMinY; y < lMaxY; y++) {
            memset(m_maskData.get() + static_cast<size_t>(y) * m_editW + lMinX, 0,
                   static_cast<size_t>(lMaxX - lMinX));
        }

        // 从所有 MosaicLayer 重建区域 mask：与区域不相交的笔迹直接跳过
        // （笔迹圆的坐标在全局像素坐标系中判断）
        const int gMinX = lMinX + m_editX;
        const int gMinY = lMinY + m_editY;
        const int gMaxX = lMaxX + m_editX;
        const int gMaxY = lMaxY + m_editY;
        for (const auto& layer : m_layerManager.GetLayers()) {
            if (const auto* mosaic = dynamic_cast<MosaicLayer*>(layer.get())) {
                const float layerBrushRadius = mosaic->GetBrushRadius();
                float scaleX = 1.0f, scaleY = 1.0f;
                if (m_displaySize.x > 0 && m_displaySize.y > 0) {
                    scaleX = GetPixelScale().x;
                    scaleY = GetPixelScale().y;
                }
                const float brushPxX = layerBrushRadius * scaleX;
                const float brushPxY = layerBrushRadius * scaleY;
                for (const auto& point : mosaic->GetPoints()) {
                    const float px = ToPixel(point).x;
                    const float py = ToPixel(point).y;
                    // 笔迹圆与区域不相交则跳过
                    if (px + brushPxX < gMinX || px - brushPxX > gMaxX ||
                        py + brushPxY < gMinY || py - brushPxY > gMaxY) {
                        continue;
                    }
                    PaintMosaicMask(point, layerBrushRadius);
                }
            }
        }
    }

    void Canvas::RefreshTexture() {
        if (!m_imageData) return;

        // 所有像素修改都限制在选区（画笔外扩最多一个半径）内：
        // 只重算/上传受影响区域，属性调整（颜色/粗细/笔刷/撤销等高频路径）
        // 不再做全图 memcpy + 全图 mask 重建 + 全图 composite + 全图上传
        const ImRect regionPx = ComputeRefreshRegionPx();

        // 建立/扩展编辑缓冲（original + 马赛克时的 mosaic/mask，选区尺寸而非全图）
        EnsureEditRegion(regionPx);
        if (m_editW == 0) return;

        // 1) 区域内从原始图像恢复
        RestoreRegionFromOriginal(regionPx);

        // 2) 区域内重建 mask
        RebuildMaskRegion(regionPx);

        // 3) 区域内合成马赛克
        CompositeMosaicRegion(regionPx);

        // 4) 只上传区域（GPU 端同帧再合并）
        UpdateTextureRegion(regionPx);
    }

    void Canvas::ConditionalRefreshTexture() {
#if !defined(SHOTLET_PLATFORM_WINDOWS)
        RefreshTexture();
#endif
    }

    void Canvas::UpdateTexture() const {
        if (!m_screenshot || !m_imageData) return;

        // 使用纹理更新回调（由 ScreenOverlay 设置，统一使用 SDL_GPU）
        if (m_textureUpdateCallback) {
            m_textureUpdateCallback(nullptr);
        }
    }

    void Canvas::UpdateTextureRegion(const ImRect& regionPx) const {
        if (!m_screenshot || !m_imageData) return;

        const int imgWidth = static_cast<int>(m_screenshotSize.x);
        const int imgHeight = static_cast<int>(m_screenshotSize.y);
        if (imgWidth <= 0 || imgHeight <= 0) return;

        TextureUpdateRegion region;
        region.x = std::max(0, static_cast<int>(std::floor(regionPx.Min.x)));
        region.y = std::max(0, static_cast<int>(std::floor(regionPx.Min.y)));
        const int maxX = std::min(imgWidth, static_cast<int>(std::ceil(regionPx.Max.x)) + 1);
        const int maxY = std::min(imgHeight, static_cast<int>(std::ceil(regionPx.Max.y)) + 1);
        region.width = maxX - region.x;
        region.height = maxY - region.y;

        if (m_textureUpdateCallback) {
            if (region.IsValid()) {
                m_textureUpdateCallback(&region);
            }
        }
    }

    std::unique_ptr<unsigned char[]> Canvas::GetImageDataCopy(int &rowPitch) const {
        rowPitch = m_imageRowPitch;
        if (!m_imageData) return nullptr;

        const int dataSize = m_imageRowPitch * static_cast<int>(m_screenshotSize.y);
        auto copy = std::make_unique<unsigned char[]>(dataSize);
        memcpy(copy.get(), m_imageData.get(), dataSize);
        return copy;
    }
} // namespace shotlet
