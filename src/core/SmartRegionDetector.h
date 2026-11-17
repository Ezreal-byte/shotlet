#pragma once
#include <imgui.h>
#include <imgui_internal.h>
#include <vector>
#include <string>

namespace shotlet {

// 检测到的智能区域（已转换为本屏幕本地坐标）
struct SmartRegion {
    ImRect bounds;          // 区域边界（本屏幕本地坐标）
    std::string label;      // 窗口名称
};

// 缓存窗口条目
// globalBounds: 全局坐标边界（用于鼠标位置匹配）
// occluders: z-order 在此窗口之上的可见窗口全局边界（用于遮挡判断）
struct CachedWindow {
    ImRect globalBounds;
    float screenOffsetX = 0.0f;
    float screenOffsetY = 0.0f;
    std::vector<ImRect> occluders;  // 在此窗口之上且与此窗口重叠的窗口
    SmartRegion localRegion;        // 转换后的本地坐标区域
};

// 收集阶段的原始窗口数据（全局坐标，按 z-order 上层在前排列）
struct RawWindow {
    ImRect globalBounds;
    std::string label;
    std::string className;
};

class SmartRegionDetector {
public:
    SmartRegionDetector() = default;

    // 一次性构建所有窗口缓存（截图完成时调用）
    // screenOffsetX/Y: 当前屏幕在全局坐标系中的偏移
    // screenW/H: 当前屏幕的逻辑尺寸（用于裁剪超出屏幕的边界）
    void BuildCache(float screenOffsetX, float screenOffsetY,
                    float screenW, float screenH);

    // 根据鼠标本地坐标查找最佳匹配区域（纯查找，无系统 API 调用）
    const SmartRegion* FindBestRegion(float localMouseX, float localMouseY) const;

    // 清除缓存
    void Clear();

    // 从平台收集的窗口构建缓存。供平台实现和单元测试复用。
    void BuildCacheFromWindows(const std::vector<RawWindow>& windows,
                               float screenOffsetX, float screenOffsetY,
                               float screenW, float screenH);

private:
    std::vector<CachedWindow> m_cache;  // 按z-order排列的窗口缓存
    SmartRegion m_screenRegion;
};

} // namespace shotlet
