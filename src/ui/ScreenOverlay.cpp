// ScreenOverlay 生命周期编排：
// 负责窗口创建/销毁的整体流程（PreCreate/SetCaptureData/ShowOverlay/Destroy/
// ResetForReuse）、Canvas/Toolbar/SubToolbar 对象的构建与析构、以及激活/暗化
// 等状态查询。SDL/ImGui 上下文细节见 ScreenOverlayContext.cpp，截图 GPU 纹理
// 上传细节见 ScreenOverlayGpuTexture.cpp，事件分发见 ScreenOverlayEvents.cpp，
// 逐帧渲染见 ScreenOverlayRender.cpp。
#include "ScreenOverlay.h"
#include "core/Canvas.h"
#include "core/CursorManager.h"
#include "platform/Appearance.h"
#include "platform/FileService.h"
#include "platform/FontProvider.h"
#include "platform/TransparentWindow.h"
#include "recording/ScreenRecorder.h"
#include "ui/ImGuiRendererFrame.h"
#include "ui/SubToolbar.h"
#include "ui/Toolbar.h"
#include "utils/Logger.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_keycode.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

#if defined(SHOTLET_PLATFORM_WINDOWS)
#include "platform/PlatformWin32.h"
#endif

namespace shotlet {

    float ScreenOverlay::ClampedUIScale(float scaleFactor) {
#if defined(SHOTLET_PLATFORM_WINDOWS)
        if (!std::isfinite(scaleFactor))
            return 1.0f;
        return std::clamp(scaleFactor, 1.0f, 3.0f);
#else
        (void) scaleFactor;
        return 1.0f;
#endif
    }

    ScreenOverlay::ScreenOverlay() = default;

    void ScreenOverlay::SetTheme(Theme theme) {
        m_config.theme = theme;
        if (m_toolbar)
            m_toolbar->SetTheme(theme);
        if (m_subToolbar)
            m_subToolbar->SetTheme(theme);
        if (m_imguiContext) {
            ImGuiContext *previous = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(m_imguiContext);
            ApplyImGuiTheme(ImGui::GetStyle(), theme);
            ImGui::SetCurrentContext(previous);
        }
        ApplyWindowTheme(m_sdlWindow, theme);
#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)
        if (m_scrollCapture)
            m_scrollCapture->SetTheme(theme);
#endif
    }

    ScreenOverlay::~ScreenOverlay() { Destroy(); }

    bool ScreenOverlay::PreCreate(const Config &config) {
        if (m_preCreated) {
            Logger::Warning("[ScreenOverlay] Already pre-created, destroy first");
            return false;
        }

        if (config.windowWidth <= 0 || config.windowHeight <= 0) {
            Logger::Error("[ScreenOverlay] Invalid window size: {}x{}", config.windowWidth, config.windowHeight);
            return false;
        }

        m_config = config;
#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)
        m_scrollPromptTitle = std::string(Tr(config.language, TextId::ScrollPromptTitle)) + "###ScrollPromptTitle";
        m_scrollRetryLoadLabel = std::string(Tr(config.language, TextId::ScrollRetryLoad)) + "###ScrollRetryLoad";
        m_scrollSaveLabel = std::string(Tr(config.language, TextId::ScrollSave)) + "###ScrollSave";
        m_scrollCloseLabel = std::string(Tr(config.language, TextId::PromptClose)) + "###PromptClose";
#endif
        auto t_pre = std::chrono::high_resolution_clock::now();

        if (!CreateNativeWindow()) {
            Logger::Error("[ScreenOverlay] Failed to create native window");
            return false;
        }
        auto t_native = std::chrono::high_resolution_clock::now();

        if (!CreateSDLWindow()) {
            Logger::Error("[ScreenOverlay] Failed to create SDL window");
            // 尚未置 m_preCreated，析构不会进入完整销毁路径，必须立即清理部分资源。
            DestroySDLWindow();
            DestroyNativeWindow();
            return false;
        }
        auto t_sdl = std::chrono::high_resolution_clock::now();

#if !defined(SHOTLET_PLATFORM_WINDOWS)
        ConfigureWindowTransparent(m_sdlWindow);
        HideSDLWindow();
#endif
        auto t_config = std::chrono::high_resolution_clock::now();

        ApplyWindowTheme(m_sdlWindow, m_config.theme);
        if (!InitializeImGui()) {
            Logger::Error("[ScreenOverlay] Failed to initialize ImGui");
            DestroySDLWindow();
            DestroyNativeWindow();
            return false;
        }
        auto t_imgui = std::chrono::high_resolution_clock::now();

        NewImGuiRendererFrame(m_gpuDevice);
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::EndFrame();
        auto t_newframe = std::chrono::high_resolution_clock::now();

        int sdlWinW = 0, sdlWinH = 0, sdlPixW = 0, sdlPixH = 0;
        SDL_GetWindowSize(m_sdlWindow, &sdlWinW, &sdlWinH);
        SDL_GetWindowSizeInPixels(m_sdlWindow, &sdlPixW, &sdlPixH);
        ImVec2 displaySize = ImGui::GetIO().DisplaySize;
        Logger::Debug("[ScreenOverlay] PreCreate: config={}x{}, scaleFactor={:.2f}, "
                      "SDL_GetWindowSize={}x{}, SDL_GetWindowSizeInPixels={}x{}, "
                      "ImGui DisplaySize={}x{}",
                      config.windowWidth, config.windowHeight, config.scaleFactor, sdlWinW, sdlWinH, sdlPixW, sdlPixH,
                      displaySize.x, displaySize.y);

        Logger::Debug("[ScreenOverlay] PreCreate timing: CreateNativeWindow={:.1f}ms, "
                      "CreateSDLWindow={:.1f}ms, ConfigureWindow+Hide={:.1f}ms, "
                      "InitializeImGui={:.1f}ms, NewFrame={:.1f}ms, total={:.1f}ms",
                      std::chrono::duration<float, std::milli>(t_native - t_pre).count(),
                      std::chrono::duration<float, std::milli>(t_sdl - t_native).count(),
                      std::chrono::duration<float, std::milli>(t_config - t_sdl).count(),
                      std::chrono::duration<float, std::milli>(t_imgui - t_config).count(),
                      std::chrono::duration<float, std::milli>(t_newframe - t_imgui).count(),
                      std::chrono::duration<float, std::milli>(t_newframe - t_pre).count());

        m_preCreated = true;
        return true;
    }

    bool ScreenOverlay::SetCaptureData(std::unique_ptr<unsigned char[]> imageData, int pixelWidth, int pixelHeight,
                                       int rowPitch) {
        if (!m_preCreated) {
            Logger::Error("[ScreenOverlay] Must PreCreate before SetCaptureData");
            return false;
        }
        if (!imageData) {
            Logger::Error("[ScreenOverlay] No image data provided");
            return false;
        }

        // swapchain 在上次 ResetForReuse 时已释放，
        // 这里（首次 GPU 使用之前）重新 claim；失败则中止本次截图会话，
        // 否则后续每帧 swapchain 获取都会失败（有底图无 UI）
        if (!EnsureGPUWindowClaimed())
            return false;

        auto t0 = std::chrono::high_resolution_clock::now();

        ImGui::SetCurrentContext(m_imguiContext);
        ImVec2 displaySize = ImGui::GetIO().DisplaySize;

        auto canvas = std::make_unique<Canvas>();
        const float uiScale = ClampedUIScale(m_config.scaleFactor);
        canvas->SetUIScale(uiScale);
        auto t_canvas = std::chrono::high_resolution_clock::now();

        // 创建 SDL_GPU 截图纹理并做首次全量上传
        if (!CreateScreenshotTexture(pixelWidth, pixelHeight, imageData.get(), rowPitch)) {
            return false;
        }
        auto t_gpu_upload = std::chrono::high_resolution_clock::now();

        ImTextureID texId = (ImTextureID) (intptr_t) m_gpuScreenshotTexture;

        canvas->SetScreenshot(texId, ImVec2(static_cast<float>(pixelWidth), static_cast<float>(pixelHeight)),
                              displaySize);
        canvas->SetScreenshotOwned(texId, ImVec2(static_cast<float>(pixelWidth), static_cast<float>(pixelHeight)),
                                   displaySize, std::move(imageData), rowPitch);

        // 设置纹理更新回调（用于马赛克/撤销等操作后更新纹理）
        // 回调只累积脏区（MergeDirtyRegion），实际 GPU 上传延迟到本帧
        // RenderFrame 开头统一执行（FlushDirtyRegion），避免高频鼠标事件
        // 每次都创建/提交 GPU command buffer
        canvas->SetTextureUpdateCallback([this](const Canvas::TextureUpdateRegion *dirtyRegion) {
            if (!m_canvas || !m_gpuScreenshotTexture)
                return;
            ImVec2 size = m_canvas->GetScreenshotSize();

            int imgWidth = static_cast<int>(size.x);
            int imgHeight = static_cast<int>(size.y);
            if (imgWidth <= 0 || imgHeight <= 0)
                return;

            if (dirtyRegion && dirtyRegion->IsValid()) {
                int uploadX = std::clamp(dirtyRegion->x, 0, imgWidth);
                int uploadY = std::clamp(dirtyRegion->y, 0, imgHeight);
                int uploadW = std::min(dirtyRegion->width, imgWidth - uploadX);
                int uploadH = std::min(dirtyRegion->height, imgHeight - uploadY);
                if (uploadW <= 0 || uploadH <= 0)
                    return;
                MergeDirtyRegion(uploadX, uploadY, uploadW, uploadH);
            } else {
                // 全图刷新
                MergeDirtyRegion(0, 0, imgWidth, imgHeight);
            }
        });

        auto toolbar = std::make_unique<Toolbar>();
        toolbar->SetLanguage(m_config.language);
        toolbar->SetTheme(m_config.theme);
        toolbar->SetCanvas(canvas.get());
        toolbar->SetTooltipFont(m_tooltipFont);
        toolbar->SetUIScale(uiScale);
#if defined(SHOTLET_PLATFORM_LINUX)
        if (!m_gpuDevice) toolbar->SetSoftwareRenderer(SDL_GetRenderer(m_sdlWindow));
#endif
        toolbar->SetConfirmCallback([this]() {
            Logger::Info("event=capture.confirm_clicked source=overlay");
            m_hasResult = true;
        });
        toolbar->SetCancelCallback([this]() { m_shouldClose = true; });
        toolbar->SetSaveCallback([this]() {
            m_saveRequested = true;
            m_hasResult = true;
        });
#if !defined(SHOTLET_PLATFORM_LINUX)
        toolbar->SetPinCallback([this]() {
            m_pinRequested = true;
            m_hasResult = true;
        });
        toolbar->SetOcrCallback([this]() {
            m_ocrRequested = true;
            m_hasResult = true;
        });
#endif
#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)
        toolbar->SetScrollCallback([this]() { m_scrollRequested = true; });
        // 录屏能力不足（如 macOS 15 以下）时不注册回调，工具栏不会显示录屏入口。
        if (IsScreenRecordingSupported()) {
            toolbar->SetRecordCallback([this]() {
                m_recordRequested = true;
                m_hasResult = true;
            });
        }
#endif

        auto subToolbar = std::make_unique<SubToolbar>();
        subToolbar->SetLanguage(m_config.language);
        subToolbar->SetTheme(m_config.theme);
        subToolbar->SetCanvas(canvas.get());
        subToolbar->SetToolbar(toolbar.get());
        subToolbar->SetTooltipFont(m_tooltipFont);
        subToolbar->SetUIScale(uiScale);

        auto t_toolbar = std::chrono::high_resolution_clock::now();

        m_smartDetector = std::make_unique<SmartRegionDetector>();
        m_smartCacheBuilt = false;

        m_cursorManager = std::make_unique<CursorManager>();
#if !defined(SHOTLET_PLATFORM_WINDOWS)
        m_cursorManager->InitSDLResources();
#endif
        canvas->SetCursorManager(m_cursorManager.get());

        auto t_end = std::chrono::high_resolution_clock::now();

        Logger::Debug("[ScreenOverlay] SetCaptureData[{}x{}] timing: "
                      "Canvas={:.1f}ms, GPU upload={:.1f}ms, Toolbar={:.1f}ms, "
                      "Cursor/Smart={:.1f}ms, total={:.1f}ms",
                      pixelWidth, pixelHeight, std::chrono::duration<float, std::milli>(t_canvas - t0).count(),
                      std::chrono::duration<float, std::milli>(t_gpu_upload - t_canvas).count(),
                      std::chrono::duration<float, std::milli>(t_toolbar - t_gpu_upload).count(),
                      std::chrono::duration<float, std::milli>(t_end - t_toolbar).count(),
                      std::chrono::duration<float, std::milli>(t_end - t0).count());

        m_canvas = std::move(canvas);
        m_toolbar = std::move(toolbar);
        m_subToolbar = std::move(subToolbar);
        m_created = true;
        return true;
    }

    void ScreenOverlay::ShowOverlay() {
        if (!m_created) {
            Logger::Error("[ScreenOverlay] Must SetCaptureData before ShowOverlay");
            return;
        }

#if defined(SHOTLET_PLATFORM_WINDOWS)
        RenderFrame();
        ShowNativeWindow();
#else
        ShowNativeWindow();
        RevealSDLWindow();
        RenderFrame();
#endif
        ClearNativeBackground();
    }

    void ScreenOverlay::Hide() {
        m_hidden = true;
        HideSDLWindow();
        HideNativeWindow();
    }

    void ScreenOverlay::ShowBackground() {
#if !defined(SHOTLET_PLATFORM_WINDOWS)
        ShowNativeWindow();
#endif
    }

    void ScreenOverlay::Destroy() {
#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)
        m_scrollCapture.reset();
#endif
        if (!m_preCreated && !m_created)
            return;

        Logger::Debug("[ScreenOverlay] Destroy begin display={}", m_config.displayId);
        Hide();
#if defined(SHOTLET_PLATFORM_WINDOWS)
        if (m_sdlWindow) {
            SDL_StopTextInput(m_sdlWindow);
        }
        ::SetCursor(::LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)));
#endif
        ReleaseCaptureResources(true);
        Logger::Debug("[ScreenOverlay] Destroy capture resources released display={}", m_config.displayId);

        if (m_preCreated) {
#if !defined(SHOTLET_PLATFORM_WINDOWS)
            if (m_gpuDevice) {
                SDL_WaitForGPUIdle(m_gpuDevice);
            }
#endif
            ShutdownImGui();
            Logger::Debug("[ScreenOverlay] Destroy ImGui shutdown display={}", m_config.displayId);
#if !defined(SHOTLET_PLATFORM_WINDOWS)
            if (m_gpuDevice) {
                SDL_WaitForGPUIdle(m_gpuDevice);
            }
#endif
            DestroySDLWindow();
            Logger::Debug("[ScreenOverlay] Destroy SDL window destroyed display={}", m_config.displayId);
            DestroyNativeWindow();
            Logger::Debug("[ScreenOverlay] Destroy native window destroyed display={}", m_config.displayId);
        }

        m_hidden = false;
        m_created = false;
        m_preCreated = false;
        m_hasResult = false;
        m_shouldClose = false;
        m_active = false;
        m_pinRequested = false;
        m_saveRequested = false;
        m_ocrRequested = false;
        m_recordRequested = false;
    }

    void ScreenOverlay::ResetForReuse() {
#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)
        m_scrollCapture.reset();
        m_scrollRequested = m_saveScrollResult = false;
        m_pendingScrollResult.reset();
        m_scrollMessage.clear();
#endif
        if (!m_preCreated && !m_created)
            return;

        Logger::Debug("[ScreenOverlay] ResetForReuse begin display={}", m_config.displayId);
        Hide();
#if defined(SHOTLET_PLATFORM_WINDOWS)
        if (m_sdlWindow) {
            SDL_StopTextInput(m_sdlWindow);
        }
        ::SetCursor(::LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW)));
#endif
        ReleaseCaptureResources(false);
        Logger::Debug("[ScreenOverlay] ResetForReuse capture resources released display={}", m_config.displayId);

        ClearNativeBackground();
#if !defined(SHOTLET_PLATFORM_WINDOWS)
        // 窗口已隐藏，默认释放 swapchain（双屏全尺寸 drawable 约 240MB）
        ReleaseGPUWindowClaim();
#endif

        m_hidden = false; // daemon 复用：解除编辑器接管时的隐藏状态，恢复渲染
        m_created = false;
        m_hasResult = false;
        m_shouldClose = false;
        m_active = false;
        m_dimmed = true;
        m_selectionLocked = false;
        m_pinRequested = false;
        m_saveRequested = false;
        m_ocrRequested = false;
        m_recordRequested = false;
        m_nsAppActivated = false;
        m_lastSmartDetectMousePos = ImVec2(-1e6f, -1e6f);
#if defined(SHOTLET_PLATFORM_WINDOWS)
        m_winLastClickTime = 0;
        m_winLastClickPosX = 0.0f;
        m_winLastClickPosY = 0.0f;
#endif
        Logger::Debug("[ScreenOverlay] ResetForReuse end display={}", m_config.displayId);
    }

    void ScreenOverlay::ReleaseGPUWindowClaim() {
#if !defined(SHOTLET_PLATFORM_WINDOWS)
        if (!m_gpuWindowClaimed || !m_gpuDevice || !m_sdlWindow)
            return;

        // 等待在途的 present/渲染完成，避免释放仍被引用的 drawable
        SDL_WaitForGPUIdle(m_gpuDevice);
        SDL_ReleaseWindowFromGPUDevice(m_gpuDevice, m_sdlWindow);
        m_gpuWindowClaimed = false;
        Logger::Debug("[ScreenOverlay] swapchain released to reclaim memory display={}", m_config.displayId);
#endif
    }

    bool ScreenOverlay::EnsureGPUWindowClaimed() {
#if !defined(SHOTLET_PLATFORM_WINDOWS)
        // 无需重新 claim（已 claim 或无 GPU 设备/窗口，后者由后续步骤失败暴露）
        if (m_gpuWindowClaimed || !m_gpuDevice || !m_sdlWindow)
            return true;

        auto t0 = std::chrono::high_resolution_clock::now();
        if (!SDL_ClaimWindowForGPUDevice(m_gpuDevice, m_sdlWindow)) {
            Logger::Error("[ScreenOverlay] re-claim window for GPU device failed: {}", SDL_GetError());
            return false;
        }
        m_gpuWindowClaimed = true;
        SDL_SetGPUSwapchainParameters(m_gpuDevice, m_sdlWindow, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                      SDL_GPU_PRESENTMODE_VSYNC);
#if defined(SHOTLET_PLATFORM_MACOS)
        ConfigureWindowTransparent(m_sdlWindow); // 重新创建的 Metal 图层必须恢复透明。
        ApplyGpuBufferCount(); // 新 swapchain 会重建 CAMetalLayer，需重新应用缓冲数
#endif
        auto t1 = std::chrono::high_resolution_clock::now();
        Logger::Debug("[ScreenOverlay] window re-claimed for GPU device display={} cost={:.1f}ms", m_config.displayId,
                      std::chrono::duration<float, std::milli>(t1 - t0).count());
        return true;
#else
        return true; // Windows 不做 swapchain 回收，视为已 claim
#endif
    }

    void ScreenOverlay::ReleaseCaptureResources(bool abandonCanvasOnWindows) {
        Logger::Debug("[ScreenOverlay] ReleaseCaptureResources begin display={}", m_config.displayId);
#if defined(SHOTLET_PLATFORM_LINUX)
        if (!m_gpuDevice && m_gpuScreenshotTexture) {
            SDL_DestroyTexture(static_cast<SDL_Texture *>(m_gpuScreenshotTexture));
            m_gpuScreenshotTexture = nullptr;
        }
#endif
        if (m_gpuDevice && m_gpuScreenshotTexture) {
#if !defined(SHOTLET_PLATFORM_WINDOWS)
            SDL_WaitForGPUIdle(m_gpuDevice);
            SDL_ReleaseGPUTexture(m_gpuDevice, static_cast<SDL_GPUTexture *>(m_gpuScreenshotTexture));
#else
            Logger::Debug("[ScreenOverlay] ReleaseCaptureResources skip GPU texture release on Windows display={}",
                          m_config.displayId);
#endif
            m_gpuScreenshotTexture = nullptr;
        }
#if !defined(SHOTLET_PLATFORM_WINDOWS)
        m_uploader.ReleaseBuffer(m_gpuDevice);
#endif
        m_uploader.Reset();
        Logger::Debug("[ScreenOverlay] ReleaseCaptureResources reset canvas display={}", m_config.displayId);
#if defined(SHOTLET_PLATFORM_WINDOWS)
        if (abandonCanvasOnWindows) {
            (void) m_canvas.release();
        } else
#endif
            m_canvas.reset();
        Logger::Debug("[ScreenOverlay] ReleaseCaptureResources reset toolbar display={}", m_config.displayId);
        m_toolbar.reset();
        Logger::Debug("[ScreenOverlay] ReleaseCaptureResources reset subToolbar display={}", m_config.displayId);
        m_subToolbar.reset();
        Logger::Debug("[ScreenOverlay] ReleaseCaptureResources reset smartDetector display={}", m_config.displayId);
        m_smartDetector.reset();
        m_smartCacheBuilt = false;
        Logger::Debug("[ScreenOverlay] ReleaseCaptureResources reset cursorManager display={}", m_config.displayId);
        m_cursorManager.reset();
        Logger::Debug("[ScreenOverlay] ReleaseCaptureResources end display={}", m_config.displayId);
    }

    bool ScreenOverlay::IsMouseInWindow() const {
#if defined(SHOTLET_PLATFORM_WINDOWS)
        if (!m_sdlWindow)
            return false;
        SDL_PropertiesID props = SDL_GetWindowProperties(m_sdlWindow);
        HWND hwnd = reinterpret_cast<HWND>(SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
        if (!hwnd)
            return false;
        POINT pt;
        GetCursorPos(&pt);
        ScreenToClient(hwnd, &pt);
        RECT rc;
        GetClientRect(hwnd, &rc);
        return PtInRect(&rc, pt);
#else
        if (!m_sdlWindow)
            return false;
        uint32_t flags = SDL_GetWindowFlags(m_sdlWindow);
        return (flags & SDL_WINDOW_MOUSE_FOCUS) != 0;
#endif
    }

    bool ScreenOverlay::HasWindowID(uint32_t windowID) const {
        return m_sdlWindow && SDL_GetWindowID(m_sdlWindow) == windowID;
    }

    void ScreenOverlay::SetActive(bool active) {
        m_active = active;
#if !defined(SHOTLET_PLATFORM_WINDOWS)
        if (m_sdlWindow && active && !m_nsAppActivated) {
            if (m_canvas && m_canvas->HasSelection()) {
                m_nsAppActivated = true;
                ForceWindowFocus(m_sdlWindow);
                SDL_StartTextInput(m_sdlWindow);
            }
        }
#endif
    }

    void ScreenOverlay::ForceActivate() { ForceWindowFocus(m_sdlWindow); }

    void ScreenOverlay::RemoveDim() { m_dimmed = false; }

} // namespace shotlet
