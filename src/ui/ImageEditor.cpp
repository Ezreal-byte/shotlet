// 独立图片编辑器：固定工具栏、可滚动图片视口、独立于标注坐标的缩放。
// 从 ScreenOverlay 的编辑器模式拆出（原 ScreenOverlayEditor.cpp 迁入），
// 持有自己的 SDL 窗口与 ImGui 上下文，基础设施（GPU 设备/字体）由外部注入。
#include "ui/ImageEditor.h"
#include "core/Canvas.h"
#include "core/CursorManager.h"
#include "ui/Toolbar.h"
#include "ui/SubToolbar.h"
#include "app/Application.h"
#include "platform/FontProvider.h"
#include "platform/TransparentWindow.h"
#include "utils/Logger.h"
#include "utils/ImGuiUtils.h"
#include "utils/WindowUtils.h"
#include "utils/ApplicationIcon.h"
#include "platform/Appearance.h"

#if defined(SHOTLET_PLATFORM_MACOS)
#include "platform/ImageEditorWindow.h"
#endif

#include <algorithm>
#include <cstring>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

namespace shotlet {

    namespace {
        // 与 ScreenOverlay::ClampedUIScale 相同的缩放钳制（Windows 按 DPI 钳制，其余平台恒 1）
        float ClampedEditorUIScale(float scaleFactor) {
#if defined(SHOTLET_PLATFORM_WINDOWS)
            if (!std::isfinite(scaleFactor)) return 1.0f;
            return std::clamp(scaleFactor, 1.0f, 3.0f);
#else
            (void)scaleFactor;
            return 1.0f;
#endif
        }
    }

    ImageEditor::ImageEditor(const ScreenOverlay::Config& config,
                             std::shared_ptr<const ScrollImage> image,
                             SDL_GPUDevice* gpuDevice)
        : m_config(config), m_gpuDevice(gpuDevice), m_image(std::move(image)) {}

    ImageEditor::ImageEditor(std::shared_ptr<const CaptureDocument> document,
                             const ScreenOverlay::Config& config, SDL_GPUDevice* gpuDevice)
        : m_config(config), m_gpuDevice(gpuDevice), m_document(std::move(document)) {}

    void ImageEditor::SetTheme(Theme theme) {
        m_config.theme = theme;
        if (m_toolbar) m_toolbar->SetTheme(theme);
        if (m_subToolbar) m_subToolbar->SetTheme(theme);
        if (m_imguiContext) {
            ImGuiContext* previous = ImGui::GetCurrentContext();
            ImGui::SetCurrentContext(m_imguiContext);
            ApplyImGuiTheme(ImGui::GetStyle(), theme);
            ImGui::SetCurrentContext(previous);
        }
        ApplyWindowTheme(m_sdlWindow, theme);
#if defined(SHOTLET_PLATFORM_MACOS)
        ApplyEditorBackground(m_sdlWindow, theme);
#endif
    }

    ImageEditor::~ImageEditor() {
        Destroy();
    }

    bool ImageEditor::Create() {
        if (m_created) {
            Logger::Warning("[ImageEditor] Already created");
            return false;
        }
        if (!m_gpuDevice) {
            Logger::Error("[ImageEditor] GPU device is null");
            return false;
        }
        std::string documentError;
        if (m_document ? !ValidateCaptureDocument(*m_document, documentError) :
            (!m_image || !IsValidPixelBufferSize(m_image->width, m_image->height, m_image->rgba.size()))) {
            Logger::Error("[ImageEditor] Invalid image or document: {}", documentError);
            return false;
        }

        if (!CreateSDLWindow()) {
            Logger::Error("[ImageEditor] Failed to create SDL window");
            return false;
        }
        ApplyWindowTheme(m_sdlWindow, m_config.theme);
        if (!InitializeImGui()) {
            Logger::Error("[ImageEditor] Failed to initialize ImGui");
            DestroySDLWindow();
            return false;
        }
        SetTheme(m_config.theme);
        // 预热字体图集（与 ScreenOverlay::PreCreate 相同的做法，避免首帧卡顿）
        ImGui::SetCurrentContext(m_imguiContext);
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        ImGui::EndFrame();

        const int imageWidth = m_document ? m_document->width : m_image->width;
        const int imageHeight = m_document ? m_document->height : m_image->height;
        const float uiScale = ClampedEditorUIScale(m_config.scaleFactor);
        m_canvas = std::make_unique<Canvas>();
        m_canvas->SetUIScale(uiScale);
        if (m_document) {
            if (!m_canvas->RestoreDocument(*m_document, 0, documentError)) {
                Logger::Error("[ImageEditor] Restore document failed: {}", documentError);
                Destroy();
                return false;
            }
        } else {
            auto pixels = std::make_unique<unsigned char[]>(m_image->rgba.size());
            std::memcpy(pixels.get(), m_image->rgba.data(), m_image->rgba.size());
            const float ratio = 1.0f / m_config.scaleFactor;
            m_canvas->ReplaceScreenshotAndSelectAll(0, ImVec2(float(imageWidth), float(imageHeight)),
                ImVec2(imageWidth * ratio, imageHeight * ratio), std::move(pixels), imageWidth * 4);
        }
        // 底图与马赛克状态全部装配后，才首次上传纹理。
        SDL_GPUTexture* gpuTex = shotlet::CreateScreenshotTexture(m_gpuDevice, imageWidth, imageHeight,
            m_canvas->GetCurrentImageData(), m_canvas->GetCurrentRowPitch());
        if (!gpuTex) {
            ShutdownImGui();
            DestroySDLWindow();
            return false;
        }
        m_gpuScreenshotTexture = gpuTex;

        m_canvas->SetScreenshot((ImTextureID)(intptr_t)m_gpuScreenshotTexture,
            m_canvas->GetScreenshotSize(), m_canvas->GetDisplaySize());
        m_canvas->SetImageEditing(true);
        m_canvas->FreezeDocumentScale();

        // 纹理更新回调（马赛克/撤销后刷新纹理）：只累积脏区，实际上传延迟到
        // RenderFrame 开头统一执行，避免高频鼠标事件每次都提交 GPU command buffer
        m_canvas->SetTextureUpdateCallback([this](const Canvas::TextureUpdateRegion* dirtyRegion) {
            if (!m_canvas || !m_gpuScreenshotTexture) return;
            const ImVec2 size = m_canvas->GetScreenshotSize();
            const int imgWidth = static_cast<int>(size.x);
            const int imgHeight = static_cast<int>(size.y);
            if (imgWidth <= 0 || imgHeight <= 0) return;

            if (dirtyRegion && dirtyRegion->IsValid()) {
                const int uploadX = std::clamp(dirtyRegion->x, 0, imgWidth);
                const int uploadY = std::clamp(dirtyRegion->y, 0, imgHeight);
                const int uploadW = std::min(dirtyRegion->width, imgWidth - uploadX);
                const int uploadH = std::min(dirtyRegion->height, imgHeight - uploadY);
                if (uploadW <= 0 || uploadH <= 0) return;
                m_uploader.Merge(uploadX, uploadY, uploadW, uploadH);
            } else {
                // 全图刷新
                m_uploader.Merge(0, 0, imgWidth, imgHeight);
            }
        });

        m_toolbar = std::make_unique<Toolbar>();
        m_toolbar->SetLanguage(m_config.language);
        m_toolbar->SetTheme(m_config.theme);
        m_toolbar->SetCanvas(m_canvas.get());
        m_toolbar->SetTooltipFont(m_tooltipFont);
        m_toolbar->SetUIScale(uiScale);
        m_toolbar->SetEditorMode(true);
#if defined(SHOTLET_PLATFORM_WINDOWS)
        m_toolbar->SetWindowActionCallback([this](Toolbar::WindowAction action) {
            switch (action) {
                case Toolbar::WindowAction::Minimize: SDL_MinimizeWindow(m_sdlWindow); break;
                case Toolbar::WindowAction::ToggleMaximize:
                    if (SDL_GetWindowFlags(m_sdlWindow) & SDL_WINDOW_MAXIMIZED) SDL_RestoreWindow(m_sdlWindow);
                    else SDL_MaximizeWindow(m_sdlWindow);
                    break;
                case Toolbar::WindowAction::Close: RequestClose(); break;
            }
        });
#endif
        m_toolbar->SetEditorActionCallback([this](Toolbar::EditorAction action) {
            if (m_dragging) return;
            m_canvas->FlushPendingInput();
            switch (action) {
                case Toolbar::EditorAction::ZoomOut:
                    m_zoomMode = 2;
                    m_transform.zoom = std::max(0.01f, m_transform.zoom / 1.25f);
                    break;
                case Toolbar::EditorAction::ZoomIn:
                    m_zoomMode = 2;
                    m_transform.zoom = std::min(4.0f, m_transform.zoom * 1.25f);
                    break;
                case Toolbar::EditorAction::ActualSize:
                    m_zoomMode = 2;
                    m_transform.zoom = 1;
                    m_resetScroll = true;
                    break;
                case Toolbar::EditorAction::Rotate:
                    m_canvas->RotateImageCounterClockwise();
                    m_zoomMode = 1;
                    m_resetScroll = true;
                    break;
                case Toolbar::EditorAction::Crop:
                    if (m_cropActive) CancelCrop();
                    else BeginCrop();
                    break;
            }
        });
        m_toolbar->SetConfirmCallback([this]() {
            if (m_cropActive) {
                // 等本帧绘制提交后再替换画布和纹理，避免工具栏回调中释放正在使用的资源。
                m_applyCropPending = true;
                return;
            }
            Logger::Info("event=capture.confirm_clicked source=editor");
            m_hasResult = true;
        });
        m_toolbar->SetCancelCallback([this]() { m_shouldClose = true; });
        m_toolbar->SetSaveCallback([this]() { m_saveRequested = true; m_hasResult = true; });
        m_toolbar->SetPinCallback([this]() { m_pinRequested = true; m_hasResult = true; });
        m_toolbar->SetOcrCallback([this]() { m_ocrRequested = true; m_hasResult = true; });

        m_subToolbar = std::make_unique<SubToolbar>();
        m_subToolbar->SetLanguage(m_config.language);
        m_subToolbar->SetTheme(m_config.theme);
        m_subToolbar->SetCanvas(m_canvas.get());
        m_subToolbar->SetToolbar(m_toolbar.get());
        m_subToolbar->SetTooltipFont(m_tooltipFont);
        m_subToolbar->SetUIScale(uiScale);

        m_cursorManager = std::make_unique<CursorManager>();
#if !defined(SHOTLET_PLATFORM_WINDOWS)
        m_cursorManager->InitSDLResources();
#endif
        m_canvas->SetCursorManager(m_cursorManager.get());

        // 小图完整等比放大到编辑区；长图继续按宽度展示并滚动浏览。
        m_zoomMode = (imageWidth < 1560 && imageHeight < 1000) ? 1 : 0;
        m_resetScroll = true;

        // 像素已复制进 Canvas，释放对拼接结果的引用
        m_image.reset();
        m_document.reset();

        SDL_StartTextInput(m_sdlWindow);
        SDL_ShowWindow(m_sdlWindow);
        ForceWindowFocus(m_sdlWindow);

        m_created = true;
        Logger::Info("[ImageEditor] Created: image={}x{}, display={}x{}, zoomMode={}",
                     m_canvas->GetScreenshotSize().x, m_canvas->GetScreenshotSize().y,
                     m_canvas->GetDisplaySize().x, m_canvas->GetDisplaySize().y, m_zoomMode);
        return true;
    }

    void ImageEditor::Destroy() {
        if (!m_sdlWindow && !m_imguiContext && !m_created) return;

        Logger::Debug("[ImageEditor] Destroy begin");
#if defined(SHOTLET_PLATFORM_WINDOWS)
        if (m_sdlWindow) {
            SDL_StopTextInput(m_sdlWindow);
        }
#endif
        // GPU 释放次序照 ScreenOverlay::Destroy：先释放截图资源（纹理/上传缓冲/
        // Canvas 等），再关闭 ImGui，最后销毁窗口
        if (m_gpuDevice && m_gpuScreenshotTexture) {
            SDL_WaitForGPUIdle(m_gpuDevice);
            SDL_ReleaseGPUTexture(m_gpuDevice, static_cast<SDL_GPUTexture*>(m_gpuScreenshotTexture));
            m_gpuScreenshotTexture = nullptr;
        }
        m_uploader.ReleaseBuffer(m_gpuDevice);
        m_canvas.reset();
        m_toolbar.reset();
        m_subToolbar.reset();
        m_cursorManager.reset();

#if !defined(SHOTLET_PLATFORM_WINDOWS)
        if (m_gpuDevice) {
            SDL_WaitForGPUIdle(m_gpuDevice);
        }
#endif
        ShutdownImGui();
#if !defined(SHOTLET_PLATFORM_WINDOWS)
        if (m_gpuDevice) {
            SDL_WaitForGPUIdle(m_gpuDevice);
        }
#endif
        DestroySDLWindow();

        m_created = false;
        m_hasResult = false;
        m_shouldClose = false;
        m_pinRequested = false;
        m_saveRequested = false;
        m_ocrRequested = false;
        Logger::Debug("[ImageEditor] Destroy end");
    }

    bool ImageEditor::HasWindowID(uint32_t windowID) const {
        return m_sdlWindow && SDL_GetWindowID(m_sdlWindow) == windowID;
    }

    ImVec2 ImageEditor::GetSelectionScreenOrigin() const {
        if (!m_sdlWindow) return ImVec2(-1, -1);
        int windowX = 0, windowY = 0;
        SDL_GetWindowPosition(m_sdlWindow, &windowX, &windowY);
        const ImRect selection = m_canvas ? m_canvas->GetSelection() : ImRect(0, 0, 0, 0);
        return EditorSelectionScreenAnchor(
            ImVec2(static_cast<float>(windowX), static_cast<float>(windowY)),
            m_transform, selection);
    }

    bool ImageEditor::CreateSDLWindow() {
        // 先以隐藏状态创建，SetupImageEditorWindow 定稿尺寸/位置后再显示，避免默认尺寸闪烁
        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
#if defined(SHOTLET_PLATFORM_WINDOWS)
        // SDL 保留 Windows 的可调整大小窗口样式，由同一顶栏承载工具和窗口控制按钮。
        flags |= SDL_WINDOW_BORDERLESS;
#endif
        m_sdlWindow = SDL_CreateWindow(Tr(m_config.language, TextId::EditorTitle), 1280, 832, flags);
        if (!m_sdlWindow) {
            Logger::Error("[ImageEditor] SDL_CreateWindow: {}", SDL_GetError());
            return false;
        }
#if defined(SHOTLET_PLATFORM_WINDOWS)
        if (!ApplicationIcon::SetWindowIcon(m_sdlWindow))
            Logger::Warning("[ImageEditor] Cannot set application icon: {}", SDL_GetError());
#endif
        // 定位到来源 overlay 所在屏幕（Setup 依据 native.screen 计算目标尺寸）
        SDL_SetWindowPosition(m_sdlWindow, m_config.windowX, m_config.windowY);

#if defined(SHOTLET_PLATFORM_MACOS)
        if (!SetupImageEditorWindow(m_sdlWindow, m_document ? m_document->width : m_image->width,
                                    m_document ? m_document->height : m_image->height, m_config.language)) {
            Logger::Error("[ImageEditor] SetupImageEditorWindow failed");
            return false;
        }
#else
#if defined(SHOTLET_PLATFORM_WINDOWS)
        m_config.scaleFactor = SDL_GetWindowDisplayScale(m_sdlWindow);
#endif
        const float uiScale = ClampedEditorUIScale(m_config.scaleFactor);
        SDL_DisplayID display = SDL_GetDisplayForWindow(m_sdlWindow);
        SDL_Rect usable;
        if (SDL_GetDisplayUsableBounds(display, &usable)) {
            const auto size = ImageEditorInitialWindowSize(
                ImVec2(float(m_document ? m_document->width : m_image->width),
                       float(m_document ? m_document->height : m_image->height)),
                ImVec2(float(usable.w), float(usable.h)),
                WindowUtils::GetDisplayScale(m_sdlWindow).pixelDensity, uiScale);
            const int width = int(size.x), height = int(size.y);
            UpdateMinimumWindowSize(uiScale);
            if (!SDL_SetWindowSize(m_sdlWindow, width, height)) return false;
            SDL_SetWindowPosition(m_sdlWindow, usable.x + (usable.w - width) / 2, usable.y + (usable.h - height) / 2);
        }
#endif
        if (!InstallWindowHitTest()) {
            Logger::Error("[ImageEditor] SDL_SetWindowHitTest: {}", SDL_GetError());
            return false;
        }

        if (!SDL_ClaimWindowForGPUDevice(m_gpuDevice, m_sdlWindow)) {
            Logger::Error("[ImageEditor] SDL_ClaimWindowForGPUDevice: {}", SDL_GetError());
            return false;
        }
        SDL_SetGPUSwapchainParameters(m_gpuDevice, m_sdlWindow,
                                      SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                      SDL_GPU_PRESENTMODE_VSYNC);
#if defined(SHOTLET_PLATFORM_MACOS)
        // platform 层不再反向依赖 app 层：GPU buffer 数在此处取值传入
        const int gpuBufferCount =
            Application::Instance() ? Application::Instance()->GetGpuBufferCount() : 2;
        ApplyEditorGpuBufferCount(m_sdlWindow, gpuBufferCount);
#endif
        return true;
    }

    void ImageEditor::UpdateMinimumWindowSize(float uiScale) {
        SDL_Rect usable;
        if (!SDL_GetDisplayUsableBounds(SDL_GetDisplayForWindow(m_sdlWindow), &usable)) return;
        const float screenInset = 2 * ImageEditorLayout::ScreenMargin * uiScale;
        SDL_SetWindowMinimumSize(m_sdlWindow,
            std::min(std::max(1, int(usable.w - screenInset)), int(ImageEditorLayout::MinimumWindowWidth * uiScale)),
            std::min(std::max(1, int(usable.h - screenInset)), int(ImageEditorLayout::MinimumWindowHeight * uiScale)));
    }

    bool ImageEditor::InstallWindowHitTest() {
        return SDL_SetWindowHitTest(m_sdlWindow, [](SDL_Window* window, const SDL_Point* point, void* data) {
            auto& editor = *static_cast<ImageEditor*>(data);
            if (!editor.m_imguiContext || !editor.m_toolbar || editor.m_dragging)
                return SDL_HITTEST_NORMAL;
            ImGuiUtils::ContextScope context(editor.m_imguiContext);
            // 原生拖动会吞掉点击；控件按压、属性面板和弹出菜单期间必须继续交给 ImGui。
            const ImVec2 position(static_cast<float>(point->x), static_cast<float>(point->y));
            if (ImGui::IsAnyItemActive() ||
                ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel) ||
                (editor.m_subToolbar && editor.m_subToolbar->GetWindowRect().Contains(position)))
                return SDL_HITTEST_NORMAL;
            const auto flags = SDL_GetWindowFlags(window);
            if ((flags & SDL_WINDOW_BORDERLESS) && (flags & SDL_WINDOW_RESIZABLE) &&
                !(flags & (SDL_WINDOW_MAXIMIZED | SDL_WINDOW_FULLSCREEN))) {
                int width = 0, height = 0;
                SDL_GetWindowSize(window, &width, &height);
                const float borderWidth = std::ceil(ImageEditorLayout::ResizeBorder * ClampedEditorUIScale(editor.m_config.scaleFactor));
                const auto resize = WindowUtils::HitTestResizeBorder(*point, width, height, borderWidth);
                if (resize != SDL_HITTEST_NORMAL) return resize;
            }
            return editor.m_toolbar->IsEditorDragArea(position) ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
        }, this);
    }

    void ImageEditor::DestroySDLWindow() {
        if (m_gpuDevice && m_sdlWindow) {
            SDL_ReleaseWindowFromGPUDevice(m_gpuDevice, m_sdlWindow);
        }
        if (m_sdlWindow) {
            SDL_DestroyWindow(m_sdlWindow);
            m_sdlWindow = nullptr;
        }
    }

    bool ImageEditor::InitializeImGui() {
        IMGUI_CHECKVERSION();
        m_imguiContext = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_imguiContext);

        ImGuiIO& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;

        const float uiScale = ClampedEditorUIScale(m_config.scaleFactor);
        float fontPixelSize = GetTextLayerFontSize() * uiScale;
        ImFontConfig fontConfig;
        fontConfig.OversampleH = 1;
        fontConfig.OversampleV = 1;
        fontConfig.FontNo = GetChineseFontNo();
        fontConfig.FontDataOwnedByAtlas = false;

        ImFont* mainFont = nullptr;
        auto fontData = GetCachedFontData();
        auto fontDataSizes = GetCachedFontDataSizes();
        if (!fontData.empty()) {
            for (size_t i = 0; i < fontData.size(); i++) {
                mainFont = io.Fonts->AddFontFromMemoryTTF(
                    const_cast<unsigned char*>(fontData[i]), fontDataSizes[i],
                    fontPixelSize, &fontConfig,
                    io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                if (mainFont) break;
            }
        } else {
            auto fontPaths = GetChineseFontPaths();
            fontConfig.FontDataOwnedByAtlas = true;
            for (const auto& path : fontPaths) {
                mainFont = io.Fonts->AddFontFromFileTTF(path.c_str(), fontPixelSize, &fontConfig,
                    io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                if (mainFont) break;
            }
        }
        if (!mainFont) mainFont = io.Fonts->AddFontDefault(&fontConfig);
        io.FontDefault = mainFont;

        m_tooltipFont = nullptr;
        float tooltipFontPixelSize = GetTooltipFontSize() * uiScale;
        ImFontConfig tooltipFontConfig;
        tooltipFontConfig.OversampleH = 1;
        tooltipFontConfig.OversampleV = 1;
        tooltipFontConfig.FontNo = GetChineseFontNo();
        tooltipFontConfig.FontDataOwnedByAtlas = false;

        if (!fontData.empty()) {
            for (size_t i = 0; i < fontData.size(); i++) {
                m_tooltipFont = io.Fonts->AddFontFromMemoryTTF(
                    const_cast<unsigned char*>(fontData[i]), fontDataSizes[i],
                    tooltipFontPixelSize, &tooltipFontConfig,
                    io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                if (m_tooltipFont) break;
            }
        } else {
            tooltipFontConfig.FontDataOwnedByAtlas = true;
            auto fontPaths = GetChineseFontPaths();
            for (const auto& path : fontPaths) {
                m_tooltipFont = io.Fonts->AddFontFromFileTTF(path.c_str(), tooltipFontPixelSize, &tooltipFontConfig,
                    io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                if (m_tooltipFont) break;
            }
        }

        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 11.0f;
        style.FrameRounding = 6.0f;
        style.GrabRounding = 6.0f;
        style.PopupRounding = 6.0f;
        style.ScrollbarRounding = 6.0f;
        style.TabRounding = 6.0f;
        ApplyImGuiTheme(style, m_config.theme);
        style.ScaleAllSizes(uiScale);

        ImGui_ImplSDL3_InitForOther(m_sdlWindow);
        ImGui_ImplSDLGPU3_InitInfo gpuInfo = {};
        gpuInfo.Device = m_gpuDevice;
        gpuInfo.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(m_gpuDevice, m_sdlWindow);
        gpuInfo.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
        gpuInfo.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
        gpuInfo.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
        ImGui_ImplSDLGPU3_Init(&gpuInfo);
        return true;
    }

    void ImageEditor::ShutdownImGui() {
        if (!m_imguiContext) return;
        ImGui::SetCurrentContext(m_imguiContext);
        ImGui_ImplSDLGPU3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(m_imguiContext);
        m_imguiContext = nullptr;
        m_tooltipFont = nullptr;
    }

    void ImageEditor::RenderFrame() {
        if (!m_created || !m_sdlWindow || !m_gpuDevice || !m_imguiContext) return;

        // 事件阶段累积的纹理脏区在此一次性上传（马赛克涂抹等高频路径）
        m_uploader.Flush(m_gpuDevice, static_cast<SDL_GPUTexture*>(m_gpuScreenshotTexture),
                         m_canvas ? m_canvas->GetCurrentImageData() : nullptr,
                         m_canvas ? m_canvas->GetCurrentRowPitch() : 0);

        ImGuiContext* prevCtx = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(m_imguiContext);

        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        RenderViewport();

        // IME 光标区域：CanvasRender::RenderInEditor 已把光标位置变换到窗口坐标，
        // 高度需随编辑视图缩放
        if (m_canvas && m_canvas->IsTextInputActive() && m_sdlWindow) {
            ImVec2 imePos = m_canvas->GetIMECursorPos();
            SDL_Rect imeRect;
            imeRect.x = static_cast<int>(imePos.x);
            imeRect.y = static_cast<int>(imePos.y);
            imeRect.w = 1;
            imeRect.h = static_cast<int>(m_canvas->GetTextFontSize() * m_transform.zoom);
            SDL_SetTextInputArea(m_sdlWindow, &imeRect, 0);
        }

        ImGui::Render();

        // 编辑器独占期间恒为活跃窗口，光标无条件应用
        if (m_cursorManager && m_canvas) {
            m_cursorManager->ApplySDL(m_canvas->GetLastCursorType());
        }

        ImDrawData* drawData = ImGui::GetDrawData();
        if (drawData && drawData->TotalVtxCount > 0) {
            SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(m_gpuDevice);
            SDL_GPUTexture* swapchainTex = nullptr;
            if (SDL_AcquireGPUSwapchainTexture(cmd, m_sdlWindow, &swapchainTex, nullptr, nullptr) && swapchainTex) {
                ImGui_ImplSDLGPU3_PrepareDrawData(drawData, cmd);

                SDL_GPUColorTargetInfo targetInfo = {};
                targetInfo.texture = swapchainTex;
                const auto background = ImGui::ColorConvertU32ToFloat4(GetThemePalette(m_config.theme).editorBackground);
                targetInfo.clear_color = SDL_FColor{background.x, background.y, background.z, background.w};
                targetInfo.load_op = SDL_GPU_LOADOP_CLEAR;
                targetInfo.store_op = SDL_GPU_STOREOP_STORE;
                targetInfo.mip_level = 0;
                targetInfo.layer_or_depth_plane = 0;
                targetInfo.cycle = false;

                SDL_GPURenderPass* renderPass = SDL_BeginGPURenderPass(cmd, &targetInfo, 1, nullptr);
                ImGui_ImplSDLGPU3_RenderDrawData(drawData, cmd, renderPass);
                SDL_EndGPURenderPass(renderPass);
            }

            SDL_SubmitGPUCommandBuffer(cmd);
        }

        if (m_applyCropPending) { m_applyCropPending = false; ApplyCrop(m_editError); }
        ImGui::SetCurrentContext(prevCtx);
    }

    void ImageEditor::RenderViewport() {
        // 原样迁自 ScreenOverlay::RenderImageEditor（ScreenOverlayEditor.cpp）
        ImGuiIO& io = ImGui::GetIO();
        const float uiScale = ClampedEditorUIScale(m_config.scaleFactor);
        const float statusHeight = ImageEditorLayout::StatusHeight * uiScale;
        // 与工具栏共用实际高度，Windows DPI 缩放后视口仍紧接工具栏下缘。
        const float top = m_toolbar->GetHeight();
#if defined(SHOTLET_PLATFORM_MACOS)
        // AppKit 在窗口缩放和跨屏时会重排标题栏，随帧同步系统按钮及其避让宽度。
        m_toolbar->SetEditorLeadingInset(LayoutImageEditorTitlebar(m_sdlWindow, top));
#endif
        m_toolbar->SetWindowMaximized((SDL_GetWindowFlags(m_sdlWindow) & SDL_WINDOW_MAXIMIZED) != 0);
        const ImVec2 region(std::max(1.0f, io.DisplaySize.x), std::max(1.0f, io.DisplaySize.y - top - statusHeight));
        m_transform.imageSize = m_canvas->GetDisplaySize();
        m_transform.quarterTurns = m_canvas->GetImageQuarterTurns();
        const ImVec2 image = m_transform.RotatedSize();
        if (m_zoomMode < 2 && !m_dragging) {
            ImVec2 available = region;
            // 长图出现纵向滚动条后，可用宽度会变窄；预留这部分，避免右侧被裁切。
            const float fitWidth = ImageEditorFitZoom(image, available, false);
            if (m_zoomMode == 0 && image.y * fitWidth + 2 * ImageEditorLayout::ViewportPadding > available.y)
                available.x -= ImGui::GetStyle().ScrollbarSize;
            m_transform.zoom = ImageEditorFitZoom(image, available, m_zoomMode == 1);
        }
        ImGui::SetNextWindowPos(ImVec2(0, top));
        ImGui::SetNextWindowSize(region);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleColor(ImGuiCol_WindowBg, ImGui::ColorConvertU32ToFloat4(GetThemePalette(m_config.theme).editorBackground));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        if (ImGui::Begin("image_editor_viewport", nullptr, flags)) {
            ImGui::BeginChild("image_editor_scroll", ImVec2(0, 0), ImGuiChildFlags_None,
                              ImGuiWindowFlags_HorizontalScrollbar |
                              (m_dragging ? ImGuiWindowFlags_NoScrollWithMouse : 0));
            if (m_resetScroll) {
                ImGui::SetScrollX(0); ImGui::SetScrollY(0);
                m_resetScroll = false;
            }
            ImGuiWindow* window = ImGui::GetCurrentWindow();
            m_viewport = window->InnerClipRect;
            const ImVec2 scaled(image.x * m_transform.zoom, image.y * m_transform.zoom);
            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            // 小图居中；大图从上方开始展示，水平和垂直方向均可滚动。
            const float left = std::max(ImageEditorLayout::ViewportPadding, (window->InnerRect.GetWidth() - scaled.x) / 2);
            const float upper = std::max(ImageEditorLayout::ViewportPadding, (window->InnerRect.GetHeight() - scaled.y) / 2);
            m_transform.origin = ImVec2(cursor.x + left, cursor.y + upper);
            ImGui::Dummy(ImVec2(std::max(window->InnerRect.GetWidth(), scaled.x + 2 * ImageEditorLayout::ViewportPadding),
                                std::max(window->InnerRect.GetHeight(), scaled.y + 2 * ImageEditorLayout::ViewportPadding)));
            ImDrawList* list = ImGui::GetWindowDrawList();
            m_canvas->RenderInEditor(list, m_transform, m_viewport);
            if (m_cropActive) {
                const auto crop = m_transform.ToScreen(m_cropRect);
                list->PushClipRect(m_viewport.Min, m_viewport.Max, true);
                list->AddRect(crop.Min, crop.Max, GetThemePalette(m_config.theme).accent, 0, 0, 2);
                list->PopClipRect();
            }
            ImGui::EndChild();
        }
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar(3);

        // 底部仅显示图片尺寸、缩放和错误信息，编辑操作集中在顶部工具栏。
        ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y - statusHeight));
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, statusHeight));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * uiScale, 5 * uiScale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0);
        if (m_tooltipFont) ImGui::PushFont(m_tooltipFont);
        ImGui::Begin("image_editor_status", nullptr, flags);
        const auto sourcePixels = m_canvas->GetScreenshotSize();
        const ImVec2 pixels = m_transform.quarterTurns % 2 ? ImVec2(sourcePixels.y, sourcePixels.x) : sourcePixels;
        ImGui::Text("%d × %d px", int(pixels.x), int(pixels.y));
        ImGui::SameLine(0, 24 * uiScale);
        ImGui::Text("%.0f%%", m_transform.zoom * 100);
        if (!m_editError.empty()) { ImGui::SameLine(); ImGui::TextUnformatted(m_editError.c_str()); }
        ImGui::End();
        if (m_tooltipFont) ImGui::PopFont();
        ImGui::PopStyleVar(3);
        m_toolbar->SetEditorActionsEnabled(!m_dragging && !m_cropDragging);
        m_toolbar->SetEditorCropping(m_cropActive);
        m_toolbar->Render();
        if (!m_cropActive) m_subToolbar->Render();
        m_toolbar->RenderWindowControls();
    }

    void ImageEditor::ProcessEvent(const SDL_Event& event) {
        // 原样迁自 ScreenOverlay::ProcessEditorEvent（ScreenOverlayEditor.cpp）
        if (!m_created || !m_imguiContext || !m_canvas) return;
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(m_imguiContext);
        ImGui_ImplSDL3_ProcessEvent(&event);
        if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
            const float oldScale = ClampedEditorUIScale(m_config.scaleFactor);
            m_config.scaleFactor = SDL_GetWindowDisplayScale(m_sdlWindow);
            const float newScale = ClampedEditorUIScale(m_config.scaleFactor);
            ImGui::GetStyle().ScaleAllSizes(newScale / oldScale);
            m_canvas->SetUIScale(newScale); m_toolbar->SetUIScale(newScale); m_subToolbar->SetUIScale(newScale);
#if defined(SHOTLET_PLATFORM_WINDOWS)
            // 跨屏后最小尺寸也要随 DPI 更新，否则可缩到工具与窗口按钮互相挤压。
            UpdateMinimumWindowSize(newScale);
#endif
        }
        if (m_cropActive && event.type == SDL_EVENT_WINDOW_FOCUS_LOST) m_cropDragging = false;
        if (m_cropActive && event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
            CancelCrop(); ImGui::SetCurrentContext(previous); return;
        }
        if (m_cropActive && (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP || event.type == SDL_EVENT_MOUSE_MOTION)) {
            ImVec2 screen;
            if (event.type == SDL_EVENT_MOUSE_MOTION) screen = ImVec2(event.motion.x, event.motion.y);
            else screen = ImVec2(event.button.x, event.button.y);
            const auto imageSize = m_canvas->GetDisplaySize();
            auto point = m_transform.ToImage(screen);
            point.x = std::clamp(point.x, 0.0f, imageSize.x); point.y = std::clamp(point.y, 0.0f, imageSize.y);
            if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT && m_viewport.Contains(screen)) {
                m_cropStart = point; m_cropDragging = true; m_cropRect = ImRect(point, point);
            }
            if (m_cropDragging) {
                m_cropRect = ImRect(ImMin(m_cropStart, point), ImMax(m_cropStart, point));
                if (event.type == SDL_EVENT_MOUSE_BUTTON_UP) m_cropDragging = false;
            }
            ImGui::SetCurrentContext(previous); return;
        }
        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && HasWindowID(event.window.windowID)) {
            RequestClose();
        } else if (event.type == SDL_EVENT_KEY_DOWN && (m_canvas->IsTextInputActive() || !ImGui::GetIO().WantTextInput)) {
            m_canvas->OnKeyDown(event.key.key, event.key.mod, event.key.repeat);
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && event.button.button == SDL_BUTTON_LEFT) {
            const ImVec2 screen(float(event.button.x), float(event.button.y));
            const bool inToolbar = m_toolbar->GetWindowRect().Contains(screen) || m_subToolbar->GetWindowRect().Contains(screen);
            const ImVec2 point = m_transform.ToImage(screen);
            if (!inToolbar && m_viewport.Contains(screen) && m_canvas->GetSelection().Contains(point)) {
                m_dragging = true;
                m_lastImagePoint = point;
                if (event.button.clicks == 2) m_canvas->OnMouseDoubleClick(point);
                else m_canvas->OnMouseDown(point);
            }
        } else if (event.type == SDL_EVENT_MOUSE_MOTION && m_dragging) {
            m_lastImagePoint = m_transform.ToImage(ImVec2(float(event.motion.x), float(event.motion.y)));
            m_canvas->OnMouseMove(m_lastImagePoint);
        } else if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == SDL_BUTTON_LEFT && m_dragging) {
            m_canvas->OnMouseUp(m_transform.ToImage(ImVec2(float(event.button.x), float(event.button.y))));
            m_dragging = false;
        } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST && m_dragging) {
            m_canvas->OnMouseUp(m_lastImagePoint);
            m_canvas->FlushPendingInput();
            m_dragging = false;
        }
        ImGui::SetCurrentContext(previous);
    }


    void ImageEditor::BeginCrop() {
        if (!m_canvas || m_dragging) return;
        m_canvas->FlushPendingInput(); m_cropActive = true; m_cropDragging = false;
        m_cropRect = ImRect(ImVec2(0, 0), m_canvas->GetDisplaySize()); m_editError.clear();
    }
    bool ImageEditor::ApplyCrop(std::string& error) {
        if (!m_cropActive || m_cropDragging || !m_canvas) return false;
        auto document = m_canvas->ExportCroppedDocument(m_cropRect, error);
        if (!document) return false;
        auto staged = std::make_unique<Canvas>();
        staged->SetUIScale(ClampedEditorUIScale(m_config.scaleFactor));
        if (!staged->RestoreDocument(*document, 0, error)) return false;
        auto* texture = CreateScreenshotTexture(m_gpuDevice, document->width, document->height,
                                                staged->GetCurrentImageData(), staged->GetCurrentRowPitch());
        if (!texture) { error = SDL_GetError(); return false; }
        m_uploader.ReleaseBuffer(m_gpuDevice);
        if (m_gpuScreenshotTexture) SDL_ReleaseGPUTexture(m_gpuDevice, static_cast<SDL_GPUTexture*>(m_gpuScreenshotTexture));
        m_gpuScreenshotTexture = texture;
        m_canvas = std::move(staged);
        m_canvas->SetScreenshot((ImTextureID)(intptr_t)texture, m_canvas->GetScreenshotSize(), m_canvas->GetDisplaySize());
        m_canvas->SetTooltipFont(m_tooltipFont); m_canvas->SetCursorManager(m_cursorManager.get());
        m_canvas->SetTextureUpdateCallback([this](const Canvas::TextureUpdateRegion* region) {
            if (region) m_uploader.Merge(region->x, region->y, region->width, region->height);
            else m_uploader.Merge(0, 0, int(m_canvas->GetScreenshotSize().x), int(m_canvas->GetScreenshotSize().y));
        });
        m_toolbar->SetCanvas(m_canvas.get()); m_subToolbar->SetCanvas(m_canvas.get());
        m_resetScroll = true; m_cropActive = false; m_zoomMode = 1; error.clear();
        return true;
    }

} // namespace shotlet
