#pragma once

// 滚动截图拼接器测试的合成数据生成器（header-only，仅测试使用）。
// 目标：可控、确定性的"长页面 + 帧序列"，覆盖设计文档 16.1 的测试场景：
// 强纹理（唯一位移）、周期纹理（位移歧义）、噪声、动态内容、固定头尾、越界背景。

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

namespace shotlet {
namespace scrolltest {

constexpr int kBlockWidth = 32;  // 色块宽度（列方向纹理粒度）

inline uint32_t Hash32(uint32_t a, uint32_t b) {
    uint32_t h = a * 0x9E3779B1u ^ b * 0x85EBCA6Bu;
    h ^= h >> 15;
    h *= 0x2545F491u;
    h ^= h >> 13;
    return h;
}

// 块亮度 40..219：由 (seed, 行键, 块号) 决定；G/B 用固定基色，
// 行内块亮度差异保证行非空白（拼接器空白行阈值为行内 max-min < 8）
inline uint8_t BlockShade(uint32_t seed, uint32_t rowKey, int block) {
    return static_cast<uint8_t>(40 + Hash32(seed, Hash32(rowKey, static_cast<uint32_t>(block))) % 180);
}

// 写一行 32px 色块纹理；rowKey 相同的行完全相同，不同则几乎必然不同
inline void WriteTexturedRow(uint8_t* row, int w, uint32_t seed, uint32_t rowKey) {
    for (int x = 0; x < w; ++x) {
        const uint8_t shade = BlockShade(seed, rowKey, x / kBlockWidth);
        row[x * 4 + 0] = shade;
        row[x * 4 + 1] = 100;
        row[x * 4 + 2] = 100;
        row[x * 4 + 3] = 0xFF;
    }
}

// 强纹理长图：行键 = 行号 → 行间签名互异（位移估计可唯一判别）
inline std::vector<uint8_t> MakeTexturedLongImage(int w, int h, uint32_t seed) {
    std::vector<uint8_t> v(static_cast<size_t>(h) * w * 4, 0);
    for (int y = 0; y < h; ++y) {
        WriteTexturedRow(v.data() + static_cast<size_t>(y) * w * 4, w, seed,
                         static_cast<uint32_t>(y));
    }
    return v;
}

// 周期行段：行键 = 行号 % period → 相隔 period 倍数的行完全相同（构造位移歧义）。
// firstRow/rowCount 描述目标行区间（长图全局行号），用于覆盖强纹理图的局部段落
inline std::vector<uint8_t> MakePeriodicRows(int w, int firstRow, int rowCount, int period,
                                             uint32_t seed) {
    std::vector<uint8_t> v(static_cast<size_t>(rowCount) * w * 4, 0);
    for (int y = 0; y < rowCount; ++y) {
        const uint32_t rowKey = static_cast<uint32_t>((firstRow + y) % period);
        WriteTexturedRow(v.data() + static_cast<size_t>(y) * w * 4, w, seed, rowKey);
    }
    return v;
}

// 固定区域（页头/页尾）：行键 = 行号的强纹理，seed 与滚动内容不同
// → 与内容行必然产生行差异，且跨帧一致（固定区域不随滚动变化）
inline std::vector<uint8_t> MakeFixedRegion(int w, int h, uint32_t seed) {
    return MakeTexturedLongImage(w, h, seed);
}

// 帧规格：按真值偏移从长图切帧（测试即"知道真值 dy"的临时 Controller）
struct FrameSpec {
    int contentY = 0;       // 内容带顶边的长图行号（可为负/越界 → 纯色背景=空白行）
    int viewportHeight = 0; // 内容带高度
    int fixedHeader = 0;    // 帧顶固定头高度
    int fixedFooter = 0;    // 帧底固定尾高度
    uint32_t headerSeed = 0x1234u;
    uint32_t footerSeed = 0x5678u;
    uint32_t noiseSeed = 0;     // 0 = 无噪声
    int noiseAmplitude = 0;     // 每通道 ± 幅度（需 ≤ 拼接器亮度容差 16）
};

constexpr uint8_t kBackgroundShade = 0x30;  // 越界背景（纯色 → 空白行）

// 切帧：固定头 + 内容带（越界补背景）+ 固定尾，可选逐像素确定性噪声
inline std::vector<uint8_t> CutFrame(const std::vector<uint8_t>& src, int srcW, int srcH,
                                     const FrameSpec& spec) {
    const int frameH = spec.fixedHeader + spec.viewportHeight + spec.fixedFooter;
    const size_t rowBytes = static_cast<size_t>(srcW) * 4;
    std::vector<uint8_t> v(static_cast<size_t>(frameH) * rowBytes, 0);

    for (int y = 0; y < frameH; ++y) {
        uint8_t* row = v.data() + static_cast<size_t>(y) * rowBytes;
        if (y < spec.fixedHeader) {
            WriteTexturedRow(row, srcW, spec.headerSeed, static_cast<uint32_t>(y));
        } else if (y >= spec.fixedHeader + spec.viewportHeight) {
            const int fy = y - spec.fixedHeader - spec.viewportHeight;
            WriteTexturedRow(row, srcW, spec.footerSeed, static_cast<uint32_t>(fy));
        } else {
            const int srcRow = spec.contentY + (y - spec.fixedHeader);
            if (srcRow >= 0 && srcRow < srcH) {
                std::memcpy(row, src.data() + static_cast<size_t>(srcRow) * rowBytes, rowBytes);
            } else {
                for (int x = 0; x < srcW * 4; x += 4) {
                    row[x + 0] = kBackgroundShade;
                    row[x + 1] = kBackgroundShade;
                    row[x + 2] = kBackgroundShade;
                    row[x + 3] = 0xFF;
                }
            }
        }
    }

    if (spec.noiseSeed != 0 && spec.noiseAmplitude > 0) {
        const int amp = spec.noiseAmplitude;
        for (int y = 0; y < frameH; ++y) {
            uint8_t* row = v.data() + static_cast<size_t>(y) * rowBytes;
            for (int x = 0; x < srcW; ++x) {
                for (int c = 0; c < 3; ++c) {
                    const int n = static_cast<int>(Hash32(spec.noiseSeed ^ (static_cast<uint32_t>(c) << 8),
                                                          Hash32(static_cast<uint32_t>(x), static_cast<uint32_t>(y))) % (2 * amp + 1)) - amp;
                    const int v0 = row[x * 4 + c];
                    row[x * 4 + c] = static_cast<uint8_t>(std::max(0, std::min(255, v0 + n)));
                }
            }
        }
    }
    return v;
}

}  // namespace scrolltest
}  // namespace shotlet
