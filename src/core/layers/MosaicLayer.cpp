#include "../Layer.h"
#include "LayerRasterUtils.h"
#include "utils/Constants.h"
#include "utils/DashedLine.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace shotlet {

// ========== MosaicLayer ==========
MosaicLayer::MosaicLayer(std::vector<ImVec2> points, int blockSize, float brushRadius)
    : m_points(std::move(points)), m_blockSize(blockSize), m_brushRadius(brushRadius) {}

void MosaicLayer::OffsetAllPoints(const ImVec2& offset) {
    for (auto& point : m_points) {
        point.x += offset.x;
        point.y += offset.y;
    }
}

ImRect MosaicLayer::GetBounds() const {
    if (m_points.empty()) return ImRect(0, 0, 0, 0);

    float minX = m_points[0].x, maxX = m_points[0].x;
    float minY = m_points[0].y, maxY = m_points[0].y;
    for (const auto& p : m_points) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    // 扩展到笔刷半径
    float r = m_brushRadius;
    return ImRect(minX - r, minY - r, maxX + r, maxY + r);
}

bool MosaicLayer::IsPointNearBorder(const ImVec2& pos, float threshold) const {
    // 按实际笔迹命中，避免选中包围框内未涂抹的空白区域；也支持单击形成的圆点。
    const float radius = m_brushRadius + std::max(0.0f, threshold);
    for (size_t i = 0; i < m_points.size(); ++i) {
        const ImVec2 start = m_points[i];
        const ImVec2 end = m_points[std::min(i + 1, m_points.size() - 1)];
        const ImVec2 segment(end.x - start.x, end.y - start.y);
        const float lengthSquared = segment.x * segment.x + segment.y * segment.y;
        const float t = lengthSquared > 0
            ? std::clamp(((pos.x - start.x) * segment.x + (pos.y - start.y) * segment.y) / lengthSquared, 0.0f, 1.0f)
            : 0.0f;
        const float dx = pos.x - start.x - t * segment.x;
        const float dy = pos.y - start.y - t * segment.y;
        if (dx * dx + dy * dy <= radius * radius) return true;
    }
    return false;
}

void MosaicLayer::RenderSelectionUI(ImDrawList* drawList, float uiScale) const {
    DrawDashedSelectionBorder(drawList, GetBounds(), false, uiScale);
}

void MosaicLayer::Render(ImDrawList* drawList, const ImVec2& canvasOffset, float uiScale) {
    (void)uiScale;
    // 马赛克预览不需要画任何东西，因为 ApplyMosaicAtPoint 已经直接应用到纹理了
    // 撤回时会通过 RefreshTexture 重新渲染
}

void MosaicLayer::RenderToImage(unsigned char* imageData, int imgWidth, int imgHeight,
                                int rowPitch, const ImVec2& canvasOffset, float scaleFactor) {
    if (m_points.empty()) return;

    int blockSize = m_blockSize * static_cast<int>(scaleFactor);
    float brushRadiusPx = m_brushRadius * scaleFactor;
    float innerRadiusPx = brushRadiusPx * 0.7f;

    // 1. 计算受影响区域的 bounding box（像素坐标）
    int bbMinX = imgWidth, bbMaxX = 0, bbMinY = imgHeight, bbMaxY = 0;
    for (const auto& point : m_points) {
        int px = static_cast<int>(point.x * scaleFactor - canvasOffset.x);
        int py = static_cast<int>(point.y * scaleFactor - canvasOffset.y);
        int r = static_cast<int>(brushRadiusPx) + blockSize;
        bbMinX = std::min(bbMinX, px - r);
        bbMaxX = std::max(bbMaxX, px + r);
        bbMinY = std::min(bbMinY, py - r);
        bbMaxY = std::max(bbMaxY, py + r);
    }
    bbMinX = std::max(0, bbMinX);
    bbMaxX = std::min(imgWidth - 1, bbMaxX);
    bbMinY = std::max(0, bbMinY);
    bbMaxY = std::min(imgHeight - 1, bbMaxY);

    int bbW = bbMaxX - bbMinX + 1;
    int bbH = bbMaxY - bbMinY + 1;
    if (bbW <= 0 || bbH <= 0) return;

    // 2. 创建临时 mask 缓冲区（单通道）
    std::vector<unsigned char> mask(bbW * bbH, 0);

    // 3. 在临时 mask 上画径向渐变圆（每个点）
    for (const auto& point : m_points) {
        float px = point.x * scaleFactor - canvasOffset.x;
        float py = point.y * scaleFactor - canvasOffset.y;

        int localMinX = std::max(0, static_cast<int>(px - brushRadiusPx) - bbMinX);
        int localMaxX = std::min(bbW - 1, static_cast<int>(px + brushRadiusPx) - bbMinX);
        int localMinY = std::max(0, static_cast<int>(py - brushRadiusPx) - bbMinY);
        int localMaxY = std::min(bbH - 1, static_cast<int>(py + brushRadiusPx) - bbMinY);

        for (int ly = localMinY; ly <= localMaxY; ly++) {
            for (int lx = localMinX; lx <= localMaxX; lx++) {
                int globalX = lx + bbMinX;
                int globalY = ly + bbMinY;
                float dist = std::sqrt((globalX - px) * (globalX - px) + (globalY - py) * (globalY - py));
                if (dist > brushRadiusPx) continue;

                float alpha;
                if (dist <= innerRadiusPx) {
                    alpha = 1.0f;
                } else {
                    float t = (dist - innerRadiusPx) / (brushRadiusPx - innerRadiusPx);
                    alpha = 1.0f - t * t * (3.0f - 2.0f * t);
                }

                // lighter 合成
                int maskIdx = ly * bbW + lx;
                int newVal = mask[maskIdx] + static_cast<int>(alpha * 255.0f);
                mask[maskIdx] = static_cast<unsigned char>(std::min(newVal, 255));
            }
        }
    }

    // 4. 先对受影响区域做马赛克像素化（保存一份副本用于 composite）
    //    需要先计算每个块的平均色，然后保存结果
    std::vector<unsigned char> mosaicCopy(bbW * bbH * 4);

    // 先拷贝受影响区域的原始像素到 mosaicCopy
    for (int y = bbMinY; y <= bbMaxY; y++) {
        memcpy(mosaicCopy.data() + (y - bbMinY) * bbW * 4,
               imageData + y * rowPitch + bbMinX * 4, bbW * 4);
    }

    // 对 mosaicCopy 做马赛克像素化（按 blockSize）
    int mosaicRowPitch = bbW * 4;
    for (int by = 0; by < bbH; by += blockSize) {
        for (int bx = 0; bx < bbW; bx += blockSize) {
            int globalBlockX = bx + bbMinX;
            int globalBlockY = by + bbMinY;

            int64_t r = 0, g = 0, b = 0, a = 0;
            int count = 0;

            for (int y = by; y < by + blockSize && y < bbH; y++) {
                for (int x = bx; x < bx + blockSize && x < bbW; x++) {
                    int idx = y * mosaicRowPitch + x * 4;
                    r += mosaicCopy[idx];
                    g += mosaicCopy[idx + 1];
                    b += mosaicCopy[idx + 2];
                    a += mosaicCopy[idx + 3];
                    count++;
                }
            }

            if (count > 0) { r /= count; g /= count; b /= count; a /= count; }

            for (int y = by; y < by + blockSize && y < bbH; y++) {
                for (int x = bx; x < bx + blockSize && x < bbW; x++) {
                    int idx = y * mosaicRowPitch + x * 4;
                    mosaicCopy[idx]     = static_cast<unsigned char>(r);
                    mosaicCopy[idx + 1] = static_cast<unsigned char>(g);
                    mosaicCopy[idx + 2] = static_cast<unsigned char>(b);
                    mosaicCopy[idx + 3] = static_cast<unsigned char>(a);
                }
            }
        }
    }

    // 5. Composite：lerp(originalImageData, mosaicCopy, mask) → imageData
    for (int y = bbMinY; y <= bbMaxY; y++) {
        for (int x = bbMinX; x <= bbMaxX; x++) {
            int maskIdx = (y - bbMinY) * bbW + (x - bbMinX);
            float maskAlpha = mask[maskIdx] / 255.0f;

            if (maskAlpha <= 0.001f) continue;  // 完全原图，跳过

            int imgIdx = y * rowPitch + x * 4;
            int mosIdx = (y - bbMinY) * mosaicRowPitch + (x - bbMinX) * 4;

            if (maskAlpha >= 0.999f) {
                // 完全马赛克
                imageData[imgIdx]     = mosaicCopy[mosIdx];
                imageData[imgIdx + 1] = mosaicCopy[mosIdx + 1];
                imageData[imgIdx + 2] = mosaicCopy[mosIdx + 2];
                imageData[imgIdx + 3] = mosaicCopy[mosIdx + 3];
            } else {
                // 混合
                float inv = 1.0f - maskAlpha;
                imageData[imgIdx]     = static_cast<unsigned char>(imageData[imgIdx]     * inv + mosaicCopy[mosIdx]     * maskAlpha);
                imageData[imgIdx + 1] = static_cast<unsigned char>(imageData[imgIdx + 1] * inv + mosaicCopy[mosIdx + 1] * maskAlpha);
                imageData[imgIdx + 2] = static_cast<unsigned char>(imageData[imgIdx + 2] * inv + mosaicCopy[mosIdx + 2] * maskAlpha);
                imageData[imgIdx + 3] = 255;
            }
        }
    }
}

std::unique_ptr<Layer> MosaicLayer::Clone() const {
    return std::make_unique<MosaicLayer>(m_points, m_blockSize, m_brushRadius);
}


} // namespace shotlet
