#pragma once

namespace shotlet::WindowChrome {

// Windows 无边框窗口共用的标题栏高度（逻辑像素）。编辑器、历史记录、设置、
// 置顶、OCR 窗口统一引用该常量，避免各处再次漂移；DPI 缩放由各窗口自行处理。
inline constexpr float TitleBarHeight = 42.0f;

// 标题栏右侧窗口控制组动作；固定尺寸窗口不提供 ToggleMaximize。
enum class WindowAction {
    Minimize,
    ToggleMaximize,
    Close,
};

} // namespace shotlet::WindowChrome
