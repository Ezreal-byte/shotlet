#pragma once
#include "i18n/Localization.h"
#include "ui/Theme.h"

#include <memory>
#include <cstdint>
#include <vector>

namespace shotlet {

// 单个屏幕的截图数据
struct ScreenData {
    int pixelWidth = 0;          // 像素宽度
    int pixelHeight = 0;         // 像素高度
    int screenX = 0;              // 全局 CG 坐标系左上角 X（逻辑点）
    int screenY = 0;              // 全局 CG 坐标系左上角 Y（逻辑点）
    int rowPitch = 0;       // 每行字节数
    float scaleFactor = 1.0f; // 像素与逻辑点的比例（Retina = 2.0）
    int displayId = -1;     // 显示器 ID
    std::unique_ptr<uint8_t[]> data; // 兼容单屏接口的自有 RGBA 数据
    const uint8_t* borrowedData = nullptr; // 多屏路径借用 ScreenOverlay/Canvas 持有的数据

    [[nodiscard]] const uint8_t* GetData() const {
        return data ? data.get() : borrowedData;
    }

    [[nodiscard]] bool OwnsData() const {
        return data != nullptr;
    }
};

// 鼠标放大镜浮层
// 120x120像素矩形，显示鼠标所在区域10倍放大
class MagnifierOverlay {

public:
    MagnifierOverlay();
    ~MagnifierOverlay();

    // 禁止拷贝
    MagnifierOverlay(const MagnifierOverlay&) = delete;
    MagnifierOverlay& operator=(const MagnifierOverlay&) = delete;

    // 显示/隐藏浮层
    void SetTheme(Theme theme) { m_theme = theme; UpdateScreenshot(); }
    void SetLanguage(Language language) { m_language = language; }
    void Show();
    void Hide();
    void SetUIScale(float scale) { m_uiScale = scale > 0.0f ? scale : 1.0f; }
    float GetUIScale() const { return m_uiScale; }

    // 是否正在显示
    bool IsVisible() const { return m_visible; }

    // 是否有缓存的屏幕数据
    bool HasScreenData() const { return !m_screens.empty(); }

    // 获取指定显示器的截图数据（返回 nullptr 如果未找到）
    const ScreenData* GetScreenData(int displayId) const;

    // 更新鼠标位置（供内部使用）
    void UpdateMousePosition(int x, int y);
    void RefreshMousePosition();

    // 设置多屏幕截图数据（移动语义，因为 ScreenData 含 unique_ptr 不可拷贝）
    void SetScreenData(std::vector<ScreenData>&& screens);

    // 清除缓存的屏幕数据
    void ClearScreenData();

    // 兼容旧接口：单屏幕截图
    void SetScreenshotData(int width, int height, const uint8_t* data, int rowPitch, int x = 0, int y = 0, float scaleFactor = 1.0f);

    // 更新截图数据
    void UpdateScreenshot();

public:
    // 初始化平台特定实现
    bool Initialize();
    void Shutdown();

    // 平台特定数据（定义为void*避免在头文件中引用Objective-C类型）
    struct Impl;
    std::unique_ptr<Impl> m_impl;

    Theme m_theme = Theme::Dark;
    Language m_language = Language::English;
    bool m_visible = false;
    float m_uiScale = 1.0f;
    int m_mouseX = 0;
    int m_mouseY = 0;

    // 当前鼠标位置的像素信息（CG 坐标系）
    int m_pixelX = 0;       // CG 坐标系 X
    int m_pixelY = 0;       // CG 坐标系 Y
    uint8_t m_pixelR = 0;   // 像素红色分量
    uint8_t m_pixelG = 0;   // 像素绿色分量
    uint8_t m_pixelB = 0;   // 像素蓝色分量

    // 复制当前色值到剪贴板
    void CopyColorToClipboard();

    // 多屏幕截图数据
    std::vector<ScreenData> m_screens;
};

} // namespace shotlet
