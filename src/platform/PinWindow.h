#pragma once
#include "i18n/Localization.h"
#include "ui/Theme.h"
namespace shotlet {

// 置顶窗口：将截图选区图片以独立置顶窗口显示在桌面上
// macOS 使用 NSWindow + NSImageView 实现，独立于 SDL/ImGui
class PinWindow {
public:
    // 新建一个置顶窗口并显示图片，保留已经打开的窗口
    // rgbaData: RGBA 格式像素数据（会被内部复制，调用后可释放）
    // width/height: 像素尺寸
    // screenX/screenY: 选区左上角在屏幕全局坐标系中的位置（逻辑点，Quartz 坐标系，Y 从上往下）
    static bool Create(const unsigned char* rgbaData, int width, int height,
                       float screenX, float screenY,
                       float scaleFactor = 1.0f,
                       Language language = Language::English,
                       Theme theme = Theme::Dark);

    // 更新所有置顶窗口的主题
    static void SetTheme(Theme theme);

    // 关闭并销毁所有置顶窗口
    static void Destroy();

    // 是否仍有打开的置顶窗口（包括最小化窗口）
    static bool IsVisible();

    // 处理所有置顶窗口的原生事件；未实现的平台为空操作。
    static void ProcessEvents();
};

} // namespace shotlet
