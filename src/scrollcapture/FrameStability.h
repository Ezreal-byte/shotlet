#pragma once

#include "scrollcapture/ScrollCaptureTypes.h"

namespace shotlet {

// 稳定帧判定的降采样差分配置（设计 10.4）。
// 阈值均为"建议初值，demo 实测校准"。
struct FrameStabilityConfig {
    int gridStep = 8;                // 降采样步长：每 gridStep 像素取 1 个采样点
    int sampleDiffThreshold = 6;     // 单采样点任一 RGB 通道差超过此值计为差异样本
    float stableDiffRatio = 0.015f;  // 差异样本占比 ≤ 此值 → 稳定（容忍视频/hover 局部动态）
};

// 两帧降采样差异率 [0,1]；尺寸不一致返回 1.0（调用方按不稳定处理）。
// 只比较 RGB，忽略 Alpha（SCK 输出 alpha 恒 255，噪声无意义）。
[[nodiscard]] float ComputeFrameDiffRatio(const PixelView& a, const PixelView& b,
                                          const FrameStabilityConfig& config = {});

// 差异率 ≤ stableDiffRatio → 稳定（设计 10.4：相邻两帧差异低于阈值才认定为稳定帧）
[[nodiscard]] bool IsFrameStable(const PixelView& a, const PixelView& b,
                                 const FrameStabilityConfig& config = {});

}  // namespace shotlet
