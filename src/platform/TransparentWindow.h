#pragma once

struct SDL_Window;

namespace shotlet {

// 设置窗口透明属性但不显示（用于 PreCreate 阶段）
void ConfigureWindowTransparent(SDL_Window* window);

// 设置窗口透明并显示（ConfigureWindowTransparent + makeKeyAndOrderFront）
void SetWindowTransparent(SDL_Window* window);

// 设置窗口不透明（恢复）
void SetWindowOpaque(SDL_Window* window);

// 强制窗口获取键盘焦点（平台特定，用于 SDL_RaiseWindow 不足时）
void ForceWindowFocus(SDL_Window* window);

// 在 NSApp 激活完成后恢复 Accessory 模式（macOS 专用）
// 返回 true 表示已成功恢复，false 表示 NSApp 尚未激活需继续等待
bool RestoreActivationPolicy();

} // namespace shotlet
