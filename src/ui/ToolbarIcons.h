#pragma once
#include <imgui.h>

namespace shotlet {

// 工具栏图标绘制组件
// 提供可复用的图标绘制函数，图标大小和颜色可通过参数传入
class ToolbarIcons {
public:
    // 工具类型枚举
    enum class IconType {
        Rectangle,
        Circle,
        Arrow,
        Brush,
        Mosaic,
        Highlight,
        Watermark,  // 水印（6 条 45° 平端斜线）
        Text,
        Number,
        Undo,
        Close,
        Ok,
        Save,
        Pin,          // 置顶（图钉）
        Ocr,          // OCR 识别（大写字母 A）
        Scroll,       // 滚动截图（竖线 + 上下双向箭头）
        Record,       // 录屏（外框 + 圆形录制标记）
        Speaker,
        Microphone,
        Camera,
        Cursor,
        Edit,         // 编辑长图（方框 + 铅笔）
        ZoomOut,
        ZoomIn,
        ActualSize,
        Rotate,
        Crop
    };

    // 绘制工具图标
    // 参数：
    //   drawList - ImDrawList 绘图列表
    //   p_min - 图标区域左上角坐标
    //   iconSize - 图标区域大小
    //   iconType - 图标类型
    //   color - 图标颜色
    //   thickness - 线宽（可选，默认1.0f）
    static void DrawIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                         IconType iconType, ImU32 color, float thickness = 1.0f);

    // 绘制矩形图标
    static void DrawRectangleIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                                   ImU32 color, float thickness = 1.0f);

    // 绘制圆形图标
    static void DrawCircleIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                                ImU32 color, float thickness = 1.0f);

    // 绘制箭头图标
    static void DrawArrowIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                               ImU32 color, float thickness = 1.5f);

    // 绘制画笔图标
    static void DrawBrushIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                               ImU32 color, float thickness = 1.0f);

    // 绘制马赛克图标
    static void DrawMosaicIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                                ImU32 color);

    // 绘制水印图标：6 条 45° 平端斜线（源自 viewBox 1024 的 SVG）
    static void DrawWatermarkIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                                   ImU32 color);

    // 绘制文字(T)图标
    static void DrawTextIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                              ImU32 color, float thickness = 1.5f);
    static void DrawNumberIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                              ImU32 color, float thickness = 1.0f);

    // 绘制撤回(Undo)图标
    static void DrawUndoIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                              ImU32 color, float thickness = 1.5f);

    // 绘制关闭(X)图标
    static void DrawCloseIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                               ImU32 color, float thickness = 1.5f);

    // 绘制确认(Ok)图标
    static void DrawOkIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                            ImU32 color, float thickness = 1.2f);

    // 绘制保存图标：顶部开口方框 + 向下箭头
    static void DrawSaveIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                              ImU32 color, float thickness = 1.5f);

    // 绘制置顶(图钉)图标：轻微圆角帽 + 竖杆
    static void DrawPinIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                             ImU32 color, float thickness = 1.5f);

    // 绘制 OCR 识别(A)图标：方形边框 + 大写字母 A
    static void DrawOcrIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                              ImU32 color, float thickness = 1.5f);

    // 绘制滚动截图图标：上下实线边框 + 左右虚线边框 + 中间上下双向线条箭头
    static void DrawScrollIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                               ImU32 color, float thickness = 1.0f);
    static void DrawEditIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                             ImU32 color, float thickness = 1.5f);
    static void DrawCropIcon(ImDrawList* drawList, const ImVec2& p_min, float iconSize,
                             ImU32 color, float thickness = 1.0f);
};

} // namespace shotlet
