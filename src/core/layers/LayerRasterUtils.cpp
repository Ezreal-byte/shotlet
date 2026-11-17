#include "LayerRasterUtils.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace shotlet {

// Bresenham 画线算法，将带宽度的线段绘制到 RGBA 图像数据上
void DrawLineToImage(unsigned char* imageData, int imgWidth, int imgHeight,
                     int rowPitch, const ImVec2& canvasOffset,
                     ImVec2 p1, ImVec2 p2, ImU32 color, float thickness) {
    // 使用浮点坐标进行抗锯齿渲染
    float fx0 = p1.x - canvasOffset.x;
    float fy0 = p1.y - canvasOffset.y;
    float fx1 = p2.x - canvasOffset.x;
    float fy1 = p2.y - canvasOffset.y;

    int x0 = static_cast<int>(std::round(fx0));
    int y0 = static_cast<int>(std::round(fy0));
    int x1 = static_cast<int>(std::round(fx1));
    int y1 = static_cast<int>(std::round(fy1));

    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    // 颜色分量
    unsigned char cr = (color >> 0) & 0xFF;
    unsigned char cg = (color >> 8) & 0xFF;
    unsigned char cb = (color >> 16) & 0xFF;
    unsigned char ca = (color >> 24) & 0xFF;

    // 圆形画笔半径（半宽度），模拟 ImGui 的抗锯齿线条
    float radius = thickness / 2.0f;
    if (radius < 0.5f) radius = 0.5f;

    // 遍历范围：覆盖画笔半径内的所有像素
    int range = static_cast<int>(std::ceil(radius));

    while (true) {
        for (int ty = -range; ty <= range; ty++) {
            for (int tx = -range; tx <= range; tx++) {
                int px = x0 + tx;
                int py = y0 + ty;
                if (px >= 0 && px < imgWidth && py >= 0 && py < imgHeight) {
                    // 计算像素到画笔中心的距离
                    float dist = std::sqrt(static_cast<float>(tx * tx + ty * ty));
                    // 抗锯齿：边缘像素使用 alpha 混合
                    float alpha = 1.0f;
                    if (dist > radius - 0.5f) {
                        alpha = std::max(0.0f, radius + 0.5f - dist);
                    }
                    if (alpha <= 0.0f) continue;

                    auto pixelAlpha = static_cast<unsigned char>(alpha * ca);
                    if (pixelAlpha == 0) continue;

                    int idx = py * rowPitch + px * 4;
                    // Alpha 混合：新颜色覆盖旧颜色
                    if (pixelAlpha == 255) {
                        imageData[idx]     = cr;
                        imageData[idx + 1] = cg;
                        imageData[idx + 2] = cb;
                        imageData[idx + 3] = ca;
                    } else {
                        unsigned char oldA = imageData[idx + 3];
                        float outA = (pixelAlpha / 255.0f) + (oldA / 255.0f) * (1.0f - pixelAlpha / 255.0f);
                        if (outA > 0.0f) {
                            float srcRatio = (pixelAlpha / 255.0f) / outA;
                            float dstRatio = 1.0f - srcRatio;
                            imageData[idx]     = static_cast<unsigned char>(cr * srcRatio + imageData[idx] * dstRatio);
                            imageData[idx + 1] = static_cast<unsigned char>(cg * srcRatio + imageData[idx + 1] * dstRatio);
                            imageData[idx + 2] = static_cast<unsigned char>(cb * srcRatio + imageData[idx + 2] * dstRatio);
                            imageData[idx + 3] = static_cast<unsigned char>(std::min(255.0f, outA * 255.0f));
                        }
                    }
                }
            }
        }
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx) { err += dx; y0 += sy; }
    }
}
// 填充三角形到 RGBA 图像数据（用于箭头头部）
void FillTriangleToImage(unsigned char* imageData, int imgWidth, int imgHeight,
                          int rowPitch, ImVec2 v0, ImVec2 v1, ImVec2 v2, ImU32 color) {
    unsigned char cr = (color >> 0) & 0xFF;
    unsigned char cg = (color >> 8) & 0xFF;
    unsigned char cb = (color >> 16) & 0xFF;
    unsigned char ca = (color >> 24) & 0xFF;

    // 计算包围盒
    int minX = static_cast<int>(std::floor(std::min({v0.x, v1.x, v2.x})));
    int maxX = static_cast<int>(std::ceil(std::max({v0.x, v1.x, v2.x})));
    int minY = static_cast<int>(std::floor(std::min({v0.y, v1.y, v2.y})));
    int maxY = static_cast<int>(std::ceil(std::max({v0.y, v1.y, v2.y})));

    minX = std::max(0, minX);
    minY = std::max(0, minY);
    maxX = std::min(imgWidth - 1, maxX);
    maxY = std::min(imgHeight - 1, maxY);

    // 预计算重心坐标参数
    float denom = (v1.y - v2.y) * (v0.x - v2.x) + (v2.x - v1.x) * (v0.y - v2.y);
    if (std::abs(denom) < 0.001f) return; // 退化三角形

    float invDenom = 1.0f / denom;
    float w0StepY = (v2.x - v1.x) * invDenom;
    float w1StepY = (v0.x - v2.x) * invDenom;
    float w0StepX = (v1.y - v2.y) * invDenom;
    float w1StepX = (v2.y - v0.y) * invDenom;

    // 起始行的重心坐标
    float w0Row = ((v1.y - v2.y) * (minX - v2.x) + (v2.x - v1.x) * (minY - v2.y)) * invDenom;
    float w1Row = ((v2.y - v0.y) * (minX - v2.x) + (v0.x - v2.x) * (minY - v2.y)) * invDenom;

    for (int y = minY; y <= maxY; y++) {
        float w0 = w0Row;
        float w1 = w1Row;
        for (int x = minX; x <= maxX; x++) {
            float w2 = 1.0f - w0 - w1;
            // 重心坐标都在 [0,1] 内表示点在三角形内
            if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f) {
                int idx = y * rowPitch + x * 4;
                if (ca == 255) {
                    imageData[idx]     = cr;
                    imageData[idx + 1] = cg;
                    imageData[idx + 2] = cb;
                    imageData[idx + 3] = ca;
                } else {
                    float srcA = ca / 255.0f;
                    float dstA = imageData[idx + 3] / 255.0f;
                    float outA = srcA + dstA * (1.0f - srcA);
                    if (outA > 0.0f) {
                        float srcRatio = srcA / outA;
                        float dstRatio = 1.0f - srcRatio;
                        imageData[idx]     = static_cast<unsigned char>(cr * srcRatio + imageData[idx] * dstRatio);
                        imageData[idx + 1] = static_cast<unsigned char>(cg * srcRatio + imageData[idx + 1] * dstRatio);
                        imageData[idx + 2] = static_cast<unsigned char>(cb * srcRatio + imageData[idx + 2] * dstRatio);
                        imageData[idx + 3] = static_cast<unsigned char>(std::min(255.0f, outA * 255.0f));
                    }
                }
            }
            w0 += w0StepX;
            w1 += w1StepX;
        }
        w0Row += w0StepY;
        w1Row += w1StepY;
    }
}

} // namespace shotlet
