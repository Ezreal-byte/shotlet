#pragma once
#include "i18n/Localization.h"
#include "ui/Theme.h"

#include <string>

namespace shotlet {

// OCR 结果窗口：左侧显示选区图片，右侧显示识别文本
// macOS 使用 NSWindow + AppKit 原生视图实现，独立于 SDL/ImGui
class OcrWindow {
public:
    // 创建 OCR 结果窗口
    // rgbaData: RGBA 像素数据（会被内部复制，调用后可释放）
    // width/height: 像素尺寸
    // text: OCR 识别文本
    // screenX/screenY: 选区左上角屏幕位置（Quartz 坐标系，Y 从上往下）
    static bool Create(const unsigned char* rgbaData, int width, int height,
                       const std::string& text,
                       float screenX, float screenY,
                       float scaleFactor = 1.0f,
                       Language language = Language::English,
                       Theme theme = Theme::Dark);

    static void SetTheme(Theme theme);
    static void Destroy();
    static bool IsVisible();
    static void ProcessEvents();
};

} // namespace shotlet
