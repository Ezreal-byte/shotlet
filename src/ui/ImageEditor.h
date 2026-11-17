#pragma once
#include "ui/ScreenOverlay.h"
#include "ui/GpuTextureUpload.h"
#include "ui/ImageEditorLayout.h"
#include "scrollcapture/ScrollCaptureController.h"
#include <memory>
#include <imgui.h>

struct ImGuiContext;
struct ImFont;
union SDL_Event;

namespace shotlet {
struct CaptureDocument;

class Canvas;
class Toolbar;
class SubToolbar;
class CursorManager;

// 独立图片编辑器：持有自己的 SDL 窗口、ImGui 上下文、Canvas/Toolbar/SubToolbar，
// 展示滚动截图拼接结果并提供标注、缩放、旋转和滚动浏览。
// 由 OverlayManager 在滚动截图完成回调中创建并管理（ScreenOverlay 不再承担编辑职责）。
class ImageEditor {
public:
    // config：来源截图 overlay 的配置（屏幕定位与 scaleFactor）
    // image：滚动拼接结果（像素在 Create() 时复制进 Canvas，此后本类不再持有）
    // gpuDevice：Application 共享 GPU 设备（注入使用，不拥有）
    ImageEditor(const ScreenOverlay::Config& config,
                std::shared_ptr<const ScrollImage> image,
                SDL_GPUDevice* gpuDevice);
    // 恢复文档沿用原逻辑坐标和绘制缩放，config.scaleFactor 仅决定当前窗口 UI。
    ImageEditor(std::shared_ptr<const CaptureDocument> document,
                const ScreenOverlay::Config& config, SDL_GPUDevice* gpuDevice);
    ~ImageEditor();
    ImageEditor(const ImageEditor&) = delete;
    ImageEditor& operator=(const ImageEditor&) = delete;

    // 一步创建（无三阶段并行需求）：窗口 + ImGui + 纹理上传 + Canvas/Toolbar 装配
    bool Create();
    void Destroy();

    // —— OverlayManager 编辑器独占分支使用 ——
    void ProcessEvent(const SDL_Event& event);
    void RenderFrame();
    void SetTheme(Theme theme);
    void RequestClose() { m_shouldClose = true; }
    bool HasWindowID(uint32_t windowID) const;

    // —— OverlayManager Is* 查询路由 ——
    bool HasResult() const { return m_hasResult; }
    bool ShouldClose() const { return m_shouldClose; }
    bool IsPinRequested() const { return m_pinRequested; }
    bool IsSaveRequested() const { return m_saveRequested; }
    bool IsOcrRequested() const { return m_ocrRequested; }

    // —— Application 结果导出（经 OverlayManager 转发）——
    Canvas* GetCanvas() { return m_canvas.get(); }
    SDL_Window* GetSDLWindow() const { return m_sdlWindow; }
    ImGuiContext* GetImGuiContext() const { return m_imguiContext; }
    const ScreenOverlay::Config& GetConfig() const { return m_config; }
    // 选区左上角的屏幕全局坐标（Pin/OCR 结果窗口锚点）：图片坐标经视图变换
    // 映射到窗口局部坐标再叠加窗口位置（EditorSelectionScreenAnchor 纯函数）
    ImVec2 GetSelectionScreenOrigin() const;
    void BeginCrop();
    void CancelCrop() { m_cropActive = false; m_cropDragging = false; m_editError.clear(); }
    bool ApplyCrop(std::string& error);
    bool IsCropping() const { return m_cropActive; }
    ImRect GetCropRect() const { return m_cropRect; }
    const ImageEditorTransform& GetViewTransform() const { return m_transform; }

    // —— 测试专用（仅测试桩 image_editor_test_impl.cpp 实现，生产构建不使用）——
    // 直接设置路由查询状态，驱动 OverlayManager 编辑器路由单测
    void SetRoutingStateForTest(bool hasResult, bool shouldClose,
                                bool pinRequested, bool saveRequested, bool ocrRequested);

private:
    void RenderViewport();   // 视口、只读状态栏和顶部工具栏
    bool CreateSDLWindow();
    bool InstallWindowHitTest();
    void UpdateMinimumWindowSize(float uiScale);
    void DestroySDLWindow();
    bool InitializeImGui();
    void ShutdownImGui();

    ScreenOverlay::Config m_config;
    SDL_GPUDevice* m_gpuDevice = nullptr;  // 注入使用，不拥有
    std::shared_ptr<const ScrollImage> m_image;  // 仅 Create() 期间使用
    std::shared_ptr<const CaptureDocument> m_document; // 恢复完成后释放，Canvas 独立持有内容
    SDL_Window* m_sdlWindow = nullptr;
    ImGuiContext* m_imguiContext = nullptr;
    void* m_gpuScreenshotTexture = nullptr;  // SDL_GPUTexture*
    DirtyRegionUploader m_uploader;

    std::unique_ptr<Canvas> m_canvas;
    std::unique_ptr<Toolbar> m_toolbar;
    std::unique_ptr<SubToolbar> m_subToolbar;
    std::unique_ptr<CursorManager> m_cursorManager;
    ImFont* m_tooltipFont = nullptr;

    // 编辑视图状态（缩放/滚动只发生在视图层，标注坐标始终为图片坐标）
    bool m_dragging = false;
    ImVec2 m_lastImagePoint{0, 0};
    bool m_resetScroll = true;
    int m_zoomMode = 0;  // 0 适应宽度，1 整图，2 手动缩放
    ImageEditorTransform m_transform;
    ImRect m_viewport = ImRect(0, 0, 0, 0);
    bool m_cropActive = false, m_cropDragging = false, m_applyCropPending = false;
    ImRect m_cropRect = ImRect(0, 0, 0, 0);
    ImVec2 m_cropStart;
    std::string m_editError;

    bool m_created = false;
    bool m_shouldClose = false;
    bool m_hasResult = false;
    bool m_pinRequested = false;
    bool m_saveRequested = false;
    bool m_ocrRequested = false;
};

} // namespace shotlet
