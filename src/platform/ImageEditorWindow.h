#pragma once
#include "i18n/Localization.h"
#include "ui/Theme.h"
struct SDL_Window;
namespace shotlet {
// 为新建的隐藏编辑窗口按目标屏幕定尺寸/位置并完成 AppKit 样式收尾。
// 窗口应先定位到来源屏幕（native.screen 即目标屏幕），创建后、显示前调用。
bool SetupImageEditorWindow(SDL_Window* window, int imageWidth, int imageHeight,
                            Language language = Language::English);
// 将原生窗口按钮对齐编辑器工具栏，返回工具栏需避让的左侧逻辑宽度。
float LayoutImageEditorTitlebar(SDL_Window* window, float toolbarHeight);

// 按调用方传入的 GPU buffer 数（Application --gpu-buffers 解析结果）设置窗口
// CAMetalLayer 的 maximumDrawableCount（窗口 claim 到 GPU 设备之后调用）。
// platform 层不反向依赖 app 层：数值由 ui 层调用方传入。
void ApplyEditorBackground(SDL_Window* window, Theme theme);
void ApplyEditorGpuBufferCount(SDL_Window* window, int count);
// 设置浏览类窗口（历史/设置）的原生外观。closeOnly 为真时仅保留关闭按钮，
// 隐藏最小化与最大化（设置窗口固定尺寸，二者无意义）。
void SetupHistoryBrowserWindow(SDL_Window* window, Theme theme, bool closeOnly = false);
// 隐藏历史记录窗口并提交原生窗口更新，避免截图捕获隐藏过渡中的残影。
void HideHistoryBrowserWindow(SDL_Window* window);
// 将原生窗口按钮垂直居中于历史记录自绘顶栏，尺寸使用窗口逻辑坐标。
void LayoutHistoryBrowserWindow(SDL_Window* window, float headerHeight);
} // namespace shotlet
