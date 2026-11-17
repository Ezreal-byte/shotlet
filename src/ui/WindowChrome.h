#pragma once
#include "utils/WindowChrome.h"
#include "Theme.h"
#include "i18n/Localization.h"

#include <functional>
#include <imgui.h>

namespace shotlet::WindowChrome {

// 窗口控制组按钮宽度（逻辑像素），与置顶窗口 GDI 按钮宽度保持一致。
inline constexpr float WindowControlButtonWidth = 46.0f;

// 控制组总宽（逻辑像素 × 界面缩放）；includeMaximize=false 时只含最小化与关闭。
float WindowControlsWidth(float uiScale, bool includeMaximize);

// 按客户区坐标判断窗口控制组命中，几何边界与绘制共用，避免依赖上一帧的悬停状态。
bool IsWindowControlsPoint(ImVec2 point, float windowWidth, float height, float uiScale, bool includeMaximize);

// 在窗口右上角渲染窗口控制组（Windows 无边框窗口共用）。按钮均分整个宽度，
// 悬停高亮，点击触发 onAction；includeMaximize=false 时省略中间的最大化按钮。
// backgroundColor 为控制组条带底色，需与调用方标题栏底色一致以保持无缝。
void RenderWindowControls(const char* windowName, float height, Theme theme, Language language,
                          float uiScale, bool maximized, bool includeMaximize, ImU32 backgroundColor,
                          ImFont* tooltipFont, const std::function<void(WindowAction)>& onAction);

} // namespace shotlet::WindowChrome
