#pragma once
#include "imgui.h"

namespace shotlet {

    // 配色只在此处定义；Theme 模块负责选择主题，业务模块只引用配色或语义颜色。
    struct ThemePalette {
        ImU32 panelBackground, editorToolbarBackground, editorBackground;
        ImU32 text, secondaryText, icon, disabledIcon, border;
        ImU32 hoverBackground, selectedBackground, accent, inactiveOption;
        ImU32 fillIcon, danger, confirm, pinActive;
        ImU32 tooltipBackground, tooltipText;
        ImU32 confirmButton, confirmButtonHovered, confirmButtonActive;
        ImU32 dangerHovered, dangerActive;
    };

    struct SettingsPalette {
        ImU32 background, sidebar, sidebarHovered, control, text, separator, recorderActive;
    };

    struct HistoryPalette {
        ImU32 background, sidebar, panel, border, text, muted, accent, link, selected, actionOverlay;
    };

    struct Colors {
        // ─── 通用颜色 ───
        static constexpr ImU32 White = IM_COL32(255, 255, 255, 255);
        static constexpr ImU32 Black = IM_COL32(0, 0, 0, 255);
        static constexpr ImU32 Transparent = IM_COL32(0, 0, 0, 0);
        static constexpr ImU32 ApplicationAccent = IM_COL32(0, 168, 107, 255);
        static constexpr ImVec4 TransparentVec4{0, 0, 0, 0};
        static constexpr float PrimaryButtonHoverOffset = .04f;

        // ─── 深浅主题 ───
        // 删除悬停使用低饱和度红色背景，与红色文字保持可读对比度。
        static constexpr ThemePalette DarkTheme{IM_COL32(30, 30, 31, 255),    IM_COL32(59, 59, 59, 255),
                                                IM_COL32(43, 43, 43, 255),    White,
                                                IM_COL32(180, 180, 180, 255), White,
                                                IM_COL32(100, 100, 100, 255), IM_COL32(49, 48, 49, 255),
                                                IM_COL32(52, 52, 53, 255),    IM_COL32(41, 41, 42, 255),
                                                IM_COL32(0, 200, 0, 255),     IM_COL32(98, 98, 99, 255),
                                                IM_COL32(122, 122, 127, 255), IM_COL32(220, 80, 80, 255),
                                                IM_COL32(72, 164, 103, 255),  IM_COL32(70, 159, 101, 255),
                                                IM_COL32(0, 0, 0, 220),       White,
                                                IM_COL32(0, 163, 99, 255),    IM_COL32(0, 186, 115, 255),
                                                IM_COL32(0, 138, 84, 255),    IM_COL32(70, 36, 37, 255),
                                                IM_COL32(89, 40, 41, 255)};
        static constexpr ThemePalette LightTheme{
                IM_COL32(248, 248, 250, 255), IM_COL32(242, 242, 245, 255), IM_COL32(228, 228, 232, 255),
                IM_COL32(32, 33, 36, 255),    IM_COL32(92, 94, 100, 255),   IM_COL32(40, 42, 46, 255),
                IM_COL32(157, 159, 165, 255), IM_COL32(204, 205, 211, 255), IM_COL32(225, 226, 232, 255),
                IM_COL32(210, 232, 221, 255), IM_COL32(0, 130, 70, 255),    IM_COL32(126, 128, 135, 255),
                IM_COL32(113, 115, 122, 255), IM_COL32(193, 45, 45, 255),   IM_COL32(30, 131, 70, 255),
                IM_COL32(30, 131, 70, 255),   IM_COL32(248, 248, 250, 245), IM_COL32(32, 33, 36, 255),
                IM_COL32(0, 130, 78, 255),    IM_COL32(0, 148, 89, 255),    IM_COL32(0, 110, 66, 255),
                IM_COL32(253, 226, 226, 255), IM_COL32(249, 203, 203, 255)};

        // ─── 设置窗口 ───
        static constexpr SettingsPalette DarkSettings{IM_COL32(30, 30, 30, 255),    IM_COL32(46, 46, 46, 255),
                                                      IM_COL32(57, 57, 57, 255),    IM_COL32(43, 43, 43, 255),
                                                      IM_COL32(222, 222, 225, 255), IM_COL32(40, 40, 40, 255),
                                                      IM_COL32(27, 52, 43, 255)};
        static constexpr SettingsPalette LightSettings{IM_COL32(250, 250, 250, 255), IM_COL32(237, 237, 237, 255),
                                                       IM_COL32(225, 225, 225, 255), IM_COL32(235, 235, 235, 255),
                                                       IM_COL32(35, 35, 38, 255),    IM_COL32(229, 229, 229, 255),
                                                       IM_COL32(219, 243, 232, 255)};
        static constexpr ImU32 SettingsAccent = ApplicationAccent;
        static constexpr ImU32 DisabledSwitch = IM_COL32(115, 115, 118, 255);
        // 录屏准备面板沿用参考图的蓝色主操作，其他窗口继续使用原有主题色。
        static constexpr ImU32 RecordingAccent = IM_COL32(48, 111, 237, 255);
        static constexpr ImU32 RecordingAccentHovered = IM_COL32(65, 127, 246, 255);
        static constexpr ImU32 RecordingAccentPressed = IM_COL32(35, 91, 204, 255);
        static constexpr ImU32 RecordingDarkBackground = IM_COL32(40, 40, 40, 255);
        static constexpr ImU32 RecordingDarkOptions = IM_COL32(55, 55, 55, 255);
        static constexpr ImU32 RecordingControlText = IM_COL32(183, 183, 183, 255);
        static constexpr ImU32 RecordingControlGrip = IM_COL32(99, 99, 99, 255);
        static constexpr ImU32 RecordingStop = IM_COL32(211, 69, 63, 255);
        static constexpr ImU32 RecordingStopHovered = IM_COL32(228, 80, 74, 255);
        static constexpr ImU32 RecordingStopPressed = IM_COL32(186, 54, 49, 255);

        // ─── 历史记录窗口 ───
        static constexpr HistoryPalette LightHistory{
                IM_COL32(248, 249, 251, 255), IM_COL32(240, 242, 245, 255), White,
                IM_COL32(211, 215, 222, 255), IM_COL32(35, 39, 45, 255),    IM_COL32(105, 111, 122, 255),
                LightTheme.confirmButton,     LightTheme.confirm,           LightTheme.selectedBackground,
                IM_COL32(231, 244, 237, 224)};
        static constexpr HistoryPalette DarkHistory{IM_COL32(23, 26, 27, 255),    IM_COL32(31, 35, 36, 255),
                                                    IM_COL32(32, 36, 37, 255),    IM_COL32(53, 59, 61, 255),
                                                    IM_COL32(217, 215, 208, 255), IM_COL32(173, 167, 155, 255),
                                                    DarkTheme.confirmButton,      DarkTheme.confirm,
                                                    IM_COL32(32, 57, 45, 255),    IM_COL32(28, 48, 39, 224)};
        static constexpr ImU32 HistoryBadgeBackground = IM_COL32(25, 29, 34, 220);

        // ─── 预设工具颜色（Canvas 选色面板） ───
        static constexpr ImU32 PresetBlue = IM_COL32(15, 173, 254, 255);
        static constexpr ImU32 PresetGreen = IM_COL32(144, 210, 2, 255);
        static constexpr ImU32 PresetYellow = IM_COL32(253, 194, 2, 255);
        static constexpr ImU32 PresetGray = IM_COL32(76, 76, 76, 255);
        static constexpr ImU32 PresetWhite = IM_COL32(254, 254, 254, 255);
        static constexpr ImU32 PresetRed = IM_COL32(249, 82, 81, 255);

        static constexpr ImU32 PRESET_COLORS[] = {
                PresetBlue, PresetGreen, PresetYellow, PresetGray, PresetWhite, PresetRed,
        };
        static constexpr int PRESET_COLOR_COUNT = 6;

        // ─── 默认颜色 ───
        static constexpr ImU32 ToolDefault = PresetRed; // 工具默认颜色（红色）
        static constexpr ImVec4 ToolDefaultVec4 = ImVec4(1.0f, 0.0f, 0.0f, 1.0f);

        // ─── 选区 ───
        static constexpr ImU32 SelectionBorder = IM_COL32(85, 190, 120, 255); // 选区边框（绿色）
        static constexpr ImU32 DimOverlay = IM_COL32(0, 0, 0, 128); // 非选区蒙层（50% 黑色）

        // ─── 选中图层边框与手柄 ───
        static constexpr ImU32 HandleFill = White; // 手柄填充（白色实色）
        static constexpr ImU32 HandleBorder = IM_COL32(116, 123, 125, 255); // 手柄描边（与选中虚线边框同色）
        static constexpr ImU32 SelectedBorder = HandleBorder; // 选中边框

        // ─── 文字图层 ───
        static constexpr ImU32 TextTransparentBg = Transparent; // 文字图层透明背景
        static constexpr ImU32 TextSelectedBorder = IM_COL32(227, 56, 24, 255); // 文字选中红色边框
        static constexpr ImU32 TextDashedBorder = HandleBorder; // 文字虚线边框
        static constexpr ImU32 TextSelectionBackground = IM_COL32(100, 150, 255, 80);

        // ─── 智能区域识别 ───
        // 复用 SelectionBorder 和 HandleFill/HandleBorder，不再需要独立颜色

        // ─── 马赛克图标棋盘 ───
        static constexpr ImU32 MosaicBlack = Black;
        static constexpr ImU32 MosaicWhite = White;

        // ─── 光标、放大镜与平台窗口 ───
        static constexpr ImU32 CursorFill = IM_COL32(255, 255, 255, 70);
        static constexpr ImU32 CursorBorder = IM_COL32(40, 40, 40, 210);
        static constexpr ImU32 MagnifierCrosshair = IM_COL32(0, 194, 117, 255);
        static constexpr ImVec4 MagnifierBorderVec4{.5f, .5f, .5f, .2f};
        // Windows DIB 使用预乘后的 BGRA，保留既有的 26/255 灰度和 51/255 不透明度。
        static constexpr ImU32 MagnifierPremultipliedBorder = IM_COL32(26, 26, 26, 51);
        static constexpr ImVec4 EditorInitialBackgroundVec4{.17f, .17f, .17f, 1};
        static constexpr ImU32 NativeCloseHovered = IM_COL32(196, 43, 28, 255);
        static constexpr ImU32 PinInitialBackground = IM_COL32(24, 24, 24, 255);
        static constexpr ImU32 DarkTrayIcon = IM_COL32(231, 231, 231, 255);
        static constexpr ImU32 LightTrayIcon = IM_COL32(40, 40, 40, 255);
        static constexpr ImU32 ScrollWarning = IM_COL32(255, 59, 48, 255);
        static constexpr ImVec4 ScrollMaskVec4{0, 0, 0, .45f};
        static constexpr ImVec4 ScrollDemoFillVec4{1, .25f, .25f, .10f};
        static constexpr ImVec4 ScrollDemoBorderVec4{1, .25f, .25f, .95f};

        // 平台位图和 Windows COLORREF 使用不同通道顺序，统一转换，避免把 ImGui 的存储顺序当作原生颜色。
        static constexpr ImU32 ToNativeRgb(ImU32 color) {
            return ((color >> IM_COL32_R_SHIFT) & 255) | (((color >> IM_COL32_G_SHIFT) & 255) << 8) |
                   (((color >> IM_COL32_B_SHIFT) & 255) << 16);
        }
        static constexpr ImU32 ToBGRA(ImU32 color) {
            return (((color >> IM_COL32_A_SHIFT) & 255) << 24) | (((color >> IM_COL32_R_SHIFT) & 255) << 16) |
                   (((color >> IM_COL32_G_SHIFT) & 255) << 8) | ((color >> IM_COL32_B_SHIFT) & 255);
        }
    };

} // namespace shotlet
