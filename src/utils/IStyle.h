#pragma once

namespace shotlet {

// 选中图层 UI 常量：虚线边框与手柄的尺寸参数
struct IStyle {
    // ─── 虚线边框 ───
    static constexpr float DashThickness = 0.5f;   // 虚线粗细
    static constexpr float DashLen       = 5.0f;    // 虚线线段长度
    static constexpr float GapLen        = 1.0f;    // 虚线间隔长度

    // ─── 手柄 ───
    static constexpr float HandleRadius  = 4.0f;          // 圆形手柄圆形半径
    static constexpr float SelectionHandleSize = 6.0f;    // 选区手柄点的大小
    static constexpr float SelectionBorderWidth = 3.5f;   // 选区边框宽度
};

} // namespace shotlet