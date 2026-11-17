#pragma once

#include <vector>
#include <string>

namespace shotlet {
    // 返回当前平台上可用的中文字体路径列表（按优先级排序）
    // ImGui (stb_truetype) 可解析的标准 .ttf/.ttc 文件
    std::vector<std::string> GetChineseFontPaths();

    // 返回中文字体在 .ttc 文件中的子字体编号（fontNo）
    int GetChineseFontNo();

    // 返回缓存的字体文件数据（预加载到内存，避免每次磁盘 I/O）
    // 返回 vector<const unsigned char*>，每个元素对应 GetChineseFontPaths() 中的一个路径
    // 调用者不应释放这些指针（由 FontProvider 管理）
    // 第一次调用时会从磁盘读取并缓存，后续调用直接返回缓存数据
    std::vector<const unsigned char*> GetCachedFontData();

    // 返回每个缓存字体数据的大小（字节）
    std::vector<int> GetCachedFontDataSizes();

    float GetTextLayerFontSize();
    float GetTopTooltipFontSize();
    float GetTooltipFontSize();
    float GetOcrFontSize();
} // namespace shotlet
