#pragma once
#include "i18n/Localization.h"
#include "Theme.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include "core/SmartRegionDetector.h"
#include "core/CursorManager.h"
#include "ui/GpuTextureUpload.h"
#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)
#include "scrollcapture/ScrollCaptureUI.h"
#endif

struct SDL_Window;
struct SDL_GPUDevice;
struct SDL_GPUTransferBuffer;
struct ImGuiContext;
struct ImFont;
union SDL_Event;

namespace shotlet {

class Canvas;
class Toolbar;
class SubToolbar;

class ScreenOverlay {
public:
    struct Config {
        int displayId = -1;
        int windowX = 0;
        int windowY = 0;
        int windowWidth = 0;
        int windowHeight = 0;
        float scaleFactor = 1.0f;
        int physicalX = 0;
        int physicalY = 0;
        int physicalWidth = 0;
        int physicalHeight = 0;
        Theme theme = Theme::Dark;
        Language language = Language::English;
    };

    ScreenOverlay();
    ~ScreenOverlay();
    ScreenOverlay(const ScreenOverlay&) = delete;
    ScreenOverlay& operator=(const ScreenOverlay&) = delete;

    // 三阶段创建（用于与截图并行执行）
    bool PreCreate(const Config& config);
    bool SetCaptureData(std::unique_ptr<unsigned char[]> imageData,
                        int pixelWidth, int pixelHeight, int rowPitch);
    void ShowOverlay();
    void ShowBackground();  // 仅显示底图 NSWindow（Phase C1：用户最快看到截图）
    void UpdateNativeBackground(const unsigned char* imageData,
                                 int pixelWidth, int pixelHeight, int rowPitch);
    void Destroy();
    void ResetForReuse();
    void ProcessEvent(const SDL_Event& event);
#if defined(SHOTLET_PLATFORM_WINDOWS)
    void ProcessGlobalMouseState(int globalX, int globalY, bool leftDown);
#endif
    void RenderFrame();
    void SetTheme(Theme theme);
#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)
    // 滚动截图完成回调：拼接结果上交（OverlayManager 注入，创建编辑器）。
    // 返回 true 表示接收成功（可清除暂存结果），false 表示加载失败（保留结果供重试/保存）
    void SetScrollImageCallback(std::function<bool(std::shared_ptr<const ScrollImage>)> cb) { m_onScrollImageReady = std::move(cb); }
    std::shared_ptr<const ScrollImage> GetConfirmedScrollImage() const {
        return m_hasResult ? m_pendingScrollResult : nullptr;
    }
    // 开始前即让出前台，避免多屏滚动采集被其他屏幕的全屏保护误取消。
    bool IsScrollCapturePendingOrActive() const { return m_scrollRequested || m_scrollCapture != nullptr; }
#endif
    void Hide();  // 窗口被独立窗口（图片编辑器）接管时隐藏并暂停渲染
    void RequestClose() { m_shouldClose = true; }
    bool IsCreated() const { return m_created; }
    bool IsPreCreated() const { return m_preCreated; }
    bool IsMouseInWindow() const;
    bool HasResult() const { return m_hasResult; }
    bool ShouldClose() const { return m_shouldClose; }
    bool HasWindowID(uint32_t windowID) const;
    Canvas* GetCanvas() { return m_canvas.get(); }
    int GetDisplayId() const { return m_config.displayId; }
    void SetActive(bool active);
    bool IsActive() const { return m_active; }
    void ForceActivate();
    bool IsDimmed() const { return m_dimmed; }
    void RemoveDim();
    void SetSelectionLocked(bool locked) { m_selectionLocked = locked; }
    bool IsSelectionLocked() const { return m_selectionLocked; }
    bool IsPinRequested() const { return m_pinRequested; }
    bool IsSaveRequested() const { return m_saveRequested; }
    bool IsOcrRequested() const { return m_ocrRequested; }
    bool IsRecordRequested() const { return m_recordRequested; }
    const Config& GetConfig() const { return m_config; }
    SDL_Window* GetSDLWindow() const { return m_sdlWindow; }
    SDL_GPUDevice* GetGPUDevice() const { return m_gpuDevice; }
    ImGuiContext* GetImGuiContext() const { return m_imguiContext; }

private:
    bool CreateNativeWindow();
    void DestroyNativeWindow();
    bool CreateSDLWindow();
    void DestroySDLWindow();
    bool InitializeImGui();
    void ShutdownImGui();
    void ReleaseCaptureResources(bool abandonCanvasOnWindows = false);
    // 创建截图 GPU 纹理并做首次全量上传（ScreenOverlayGpuTexture.cpp）
    bool CreateScreenshotTexture(int pixelWidth, int pixelHeight, const unsigned char* data, int rowPitch);
    // 帧内脏区合并上传（ScreenOverlayGpuTexture.cpp）：事件阶段 MergeDirtyRegion
    // 只累积矩形，RenderFrame 开头 FlushDirtyRegion 一次性上传，避免马赛克涂抹时
    // 每个鼠标事件都创建/提交 GPU command buffer
    void MergeDirtyRegion(int x, int y, int w, int h);
    void FlushDirtyRegion();
    // Windows 下限制 UI 缩放范围，其他平台恒为 1.0
    static float ClampedUIScale(float scaleFactor);
    void ShowNativeWindow();
    void HideNativeWindow();
    void ClearNativeBackground();
    void HideSDLWindow();    // 平台特定：隐藏 SDL 窗口（PreCreate 阶段）
    void RevealSDLWindow();  // 平台特定：显示并激活 SDL 窗口（ShowOverlay 阶段）
    // 非 Windows 平台截图结束后默认释放 swapchain（回收 drawable 内存），
    // 下次截图 SetCaptureData 时由 EnsureGPUWindowClaimed 重新 claim
    void ReleaseGPUWindowClaim();
    // 重新 claim 失败返回 false（调用方 SetCaptureData 须中止会话）
    bool EnsureGPUWindowClaimed();
#if defined(SHOTLET_PLATFORM_MACOS)
    // 按 Application::GetGpuBufferCount 设置 CAMetalLayer maximumDrawableCount
    // （swapchain 每次 claim 后都会新建 layer，故每次 claim 后都要调用）
    void ApplyGpuBufferCount();
#endif
#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)
    void PollScrollCapture();
    std::unique_ptr<ScrollCaptureUI> m_scrollCapture;
    bool m_scrollRequested = false;
    bool m_saveScrollResult = false;
    std::shared_ptr<const ScrollImage> m_pendingScrollResult;
    std::string m_scrollMessage;
    // 会话语言不变，预先缓存带稳定 ImGui ID 的标题与按钮文案。
    std::string m_scrollPromptTitle, m_scrollRetryLoadLabel, m_scrollSaveLabel, m_scrollCloseLabel;
    std::function<bool(std::shared_ptr<const ScrollImage>)> m_onScrollImageReady;
#endif

    bool m_hidden = false;  // 被独立窗口（编辑器）接管时暂停渲染
    Config m_config;
    bool m_created = false, m_active = false, m_dimmed = true;
    bool m_selectionLocked = false, m_pinRequested = false, m_saveRequested = false, m_ocrRequested = false;
    bool m_recordRequested = false;
    bool m_shouldClose = false, m_hasResult = false, m_nsAppActivated = false;
    bool m_preCreated = false;
#if defined(SHOTLET_PLATFORM_WINDOWS)
    bool m_winLeftWasDown = false;
    bool m_winMouseDownOnCanvas = false;
    uint32_t m_winLastClickTime = 0;    // 上次左键按下的时间戳（用于双击检测）
    float m_winLastClickPosX = 0.0f;    // 上次左键按下的 X（用于双击位置检测）
    float m_winLastClickPosY = 0.0f;    // 上次左键按下的 Y（用于双击位置检测）
#endif

    void* m_nativeWindow = nullptr;
    SDL_Window* m_sdlWindow = nullptr;
    SDL_GPUDevice* m_gpuDevice = nullptr;
    bool m_gpuWindowClaimed = false;  // 窗口当前是否已 claim 到 GPU 设备（复用时会释放并重新 claim）
    void* m_gpuScreenshotTexture = nullptr;  // SDL_GPUTexture* 截图纹理（用于回调更新）
    // 帧内待上传的纹理脏区 + 常驻上传 transfer buffer（事件阶段累积，渲染帧统一刷新）
    DirtyRegionUploader m_uploader;
    ImGuiContext* m_imguiContext = nullptr;

    std::unique_ptr<Canvas> m_canvas;
    std::unique_ptr<Toolbar> m_toolbar;
    std::unique_ptr<SubToolbar> m_subToolbar;
    ImFont* m_tooltipFont = nullptr;
    std::unique_ptr<SmartRegionDetector> m_smartDetector;
    bool m_smartCacheBuilt = false;
    ImVec2 m_lastSmartDetectMousePos = ImVec2(-1e6f, -1e6f);
    std::unique_ptr<CursorManager> m_cursorManager;
};

} // namespace shotlet
