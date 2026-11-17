#pragma once
#include <imgui.h>
#include "ScreenOverlay.h"
#include "platform/MagnifierOverlay.h"
#include <vector>
#include <memory>

namespace shotlet {

class IScreenCapture;
class ImageEditor;
class Canvas;
struct ScrollImage;
struct CaptureDocument;
#if defined(SHOTLET_PLATFORM_WINDOWS)
class WindowsCaptureGuard;
#endif

// 管理所有屏幕的 Overlay 窗口生命周期
// 负责：创建/销毁 ScreenOverlay、检测活跃屏幕、非阻塞帧循环；
// 以及滚动截图完成后创建并独占管理图片编辑器（ImageEditor）
class OverlayManager {
public:
    OverlayManager();
    ~OverlayManager();

    // 禁止拷贝
    OverlayManager(const OverlayManager&) = delete;
    OverlayManager& operator=(const OverlayManager&) = delete;

    // 创建所有屏幕的 Overlay
    // capture: 截图接口，用于获取屏幕数据和显示器信息
    bool CreateAll(IScreenCapture* capture, Language language = Language::English, Theme theme = Theme::Dark);
    void SetTheme(Theme theme);
    bool OpenDocument(std::shared_ptr<const CaptureDocument> document, const ScreenOverlay::Config& config, SDL_GPUDevice* device);
    void SetExternalEventCallback(std::function<bool(const SDL_Event&)> callback) { m_externalEvent = std::move(callback); }
    void SetThemeChangedCallback(std::function<void()> callback) { m_themeChanged = std::move(callback); }

    // 预热 Overlay 窗口和 ImGui/SDL_GPU 路径，创建后立即销毁，不显示窗口。
    static void WarmupOverlays(IScreenCapture* capture);

    // 销毁所有 Overlay
    void DestroyAll();
    void ResetForReuse();
    // 先隐藏截图和编辑窗口，再等待 GPU 和释放资源；不影响贴图/OCR 结果窗口。
    void HideAll();

    // 每帧调用：更新活跃屏幕 + 渲染所有 Overlay
    void ProcessFrame();

    // 获取当前活跃的 Overlay（鼠标所在屏幕）
    ScreenOverlay* GetActiveOverlay() const;

    // 获取指定 displayId 的 Overlay
    ScreenOverlay* GetOverlay(int displayId) const;

    // 获取所有 Overlay
    const std::vector<std::unique_ptr<ScreenOverlay>>& GetOverlays() const { return m_overlays; }

    // 检查是否有活跃的 Overlay 完成了选区
    bool IsFinished() const;

    // 检查是否所有 Overlay 都取消了（ESC）
    bool IsCancelled() const;

    // 检查是否有活跃的 Overlay 请求了置顶
    bool IsPinRequested() const;

    // 检查是否有活跃的 Overlay 请求了 OCR
    bool IsOcrRequested() const;

    // 检查是否有活跃的 Overlay 请求了保存
    bool IsSaveRequested() const;
    bool IsRecordRequested() const;

    // 是否已创建
    bool IsCreated() const { return m_imageEditor != nullptr || !m_overlays.empty(); }

    // 获取活跃屏幕的 displayId
    int GetActiveDisplayId() const { return m_activeDisplayId; }

    // —— 结果导出上下文（供 Application 导出截图/长图）——
    // 图片编辑器独占时路由到编辑器，否则取活跃 overlay；无来源时返回空
    Canvas* GetResultCanvas() const;
    SDL_Window* GetResultSDLWindow() const;
    ImGuiContext* GetResultImGuiContext() const;
    const ScreenOverlay::Config* GetResultConfig() const;
    // 选区左上角的屏幕全局坐标（Pin/OCR 结果窗口锚点）：overlay 路径选区即
    // 屏幕坐标（窗口原点+选区）；编辑器路径选区为图片坐标，经编辑器视图变换
    // 后叠加编辑器窗口位置。须在结果导出（有选区）时调用，无来源返回 (-1,-1)。
    ImVec2 GetResultSelectionScreenOrigin() const;

    // 测试专用方法（仅在测试实现中有效）
    void AddTestOverlay(std::unique_ptr<ScreenOverlay> overlay);
    void SetActiveDisplayId(int id);
    void SetMagnifierVisible(bool visible);
    bool IsMagnifierVisible() const;
    // 安装编辑器驱动路由测试（生产由滚动截图结果创建，测试不可达）
    void SetImageEditorForTest(std::unique_ptr<ImageEditor> editor);

private:
    // 检测鼠标在哪个屏幕上，更新活跃 Overlay（anyHasSelection 由调用方传入，避免重复扫描）
    void UpdateActiveScreen(bool anyHasSelection);
#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)
    // 滚动截图完成回调入口：同步创建编辑器并隐藏 overlay。
    // 失败返回 false（来源 overlay 保留结果供重试/保存）
    bool HandleScrollImage(std::shared_ptr<const ScrollImage> image, ScreenOverlay* source);
#endif
    // 隐藏所有 overlay 与放大镜，进入编辑器独占
    void EnterImageEditor();
    // 鼠标命中测试：返回鼠标所在的 overlay（Windows 用物理像素，macOS 用逻辑点坐标）
    ScreenOverlay* HitTestMouseOverlay(float mouseX, float mouseY) const;
    void ActivateOverlay(ScreenOverlay* overlay, bool forceFocus);
    void SyncActiveOverlayToSelection();
#if defined(SHOTLET_PLATFORM_WINDOWS)
    // 显示首屏前建立保护；刷新心跳时接管保护线程的取消请求。
    bool StartCaptureGuard();
    bool UpdateCaptureGuard();
#endif

    std::function<void()> m_themeChanged;
    std::function<bool(const SDL_Event&)> m_externalEvent;
    std::vector<std::unique_ptr<ScreenOverlay>> m_overlays;
    std::unique_ptr<ImageEditor> m_imageEditor;  // 滚动截图完成后的独立图片编辑器
    std::unique_ptr<MagnifierOverlay> m_magnifier;
    int m_activeDisplayId = -1;
    ImVec2 m_initialMousePos = ImVec2(-1, -1);  // 启动时的鼠标位置，用于判断是否移动过
    bool m_mouseHasMoved = false;               // 鼠标是否真正移动过
    bool m_forceActivatePending = false;        // CreateAll 完成后待激活窗口
#if defined(SHOTLET_PLATFORM_WINDOWS)
    std::unique_ptr<WindowsCaptureGuard> m_captureGuard;
    bool m_winEscWasDown = false, m_winCtrlCWasDown = false;
#endif
};

} // namespace shotlet
