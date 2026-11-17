#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>

namespace shotlet {

// 只读 RGBA8 像素视图：不拥有数据，调用方保证其在调用期间有效
struct PixelView {
    const uint8_t* data = nullptr;  // RGBA8，左上角为原点
    int width = 0;
    int height = 0;
    int pitch = 0;  // 每行字节数（≥ width*4，允许行对齐填充）

    // 取第 y 行行首指针
    [[nodiscard]] const uint8_t* Row(int y) const {
        return data + static_cast<size_t>(y) * static_cast<size_t>(pitch);
    }
};

// 矩形：全局显示坐标（CGEvent/CGDisplayBounds 同款左上原点），**点**单位。
// 像素换算由采集/驱动层按所在显示器缩放系数完成。
struct GlobalRect {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
};

// 长图统一上限（设计文档 3.4 / 12.2）
namespace ScrollCaptureLimits {
constexpr int64_t kMaxImageHeight = 16384;      // 最大像素高度（Metal 单张 2D 纹理常见上限）
constexpr int64_t kMaxRgbaBytes = 256LL << 20;  // 最大 RGBA 内存（width * height * 4）
}  // namespace ScrollCaptureLimits

// 平台后端每次反向输入只向 Controller 通知一次，避免空闲轮询重复转发。
// 返回 0 表示无需转发。
inline int64_t ConsumeReverseNotice(std::atomic<bool>& reversed, int inputDirection) {
    if (!reversed.exchange(false)) return 0;
    return inputDirection != 0 ? -inputDirection : 0;
}

}  // namespace shotlet
