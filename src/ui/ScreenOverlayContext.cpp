// ScreenOverlay SDL_GPU + ImGui 上下文管理：
// 负责 SDL 窗口的创建/销毁（含 GPU 设备绑定与 swapchain 参数）、
// 以及 ImGui 上下文的初始化（中文字体加载 fallback 链、样式配置）与销毁。
#include "ScreenOverlay.h"
#include "app/Application.h"
#include "platform/FontProvider.h"
#include "utils/Logger.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#if defined(SHOTLET_PLATFORM_LINUX)
#include <imgui_impl_sdlrenderer3.h>
#endif

namespace shotlet {

    bool ScreenOverlay::CreateSDLWindow() {
        SDL_WindowFlags flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        // 创建成功并准备好首帧之前保持隐藏，失败时不能留下拦截输入的全屏窗口。
        flags |= SDL_WINDOW_HIDDEN;

        std::string windowName = "Shotlet_" + std::to_string(m_config.displayId);
        m_sdlWindow = SDL_CreateWindow(windowName.c_str(), m_config.windowWidth, m_config.windowHeight, flags);
        if (!m_sdlWindow) {
            Logger::Error("[ScreenOverlay] SDL_CreateWindow: {}", SDL_GetError());
            return false;
        }
        SDL_SetWindowPosition(m_sdlWindow, m_config.windowX, m_config.windowY);

        // 获取共享的 GPU 设备
        m_gpuDevice = Application::Instance() ? Application::Instance()->GetGPUDevice() : nullptr;
#if defined(SHOTLET_PLATFORM_LINUX)
        if (!m_gpuDevice) {
            auto *renderer = SDL_CreateRenderer(m_sdlWindow, "software");
            if (!renderer) {
                Logger::Error("[ScreenOverlay] SDL software renderer: {}", SDL_GetError());
                return false;
            }
            // SDL emulates pacing when the window surface cannot provide vsync.
            // The capture loop otherwise has no idle delay and can saturate a CPU.
            if (!SDL_SetRenderVSync(renderer, 1))
                Logger::Warning("[ScreenOverlay] Cannot pace software frames: {}", SDL_GetError());
            Logger::Info("event=overlay.renderer backend=software display={}", m_config.displayId);
            return true;
        }
#endif
        if (!m_gpuDevice) {
            Logger::Error("[ScreenOverlay] GPU device is null");
            return false;
        }

        // 声明窗口使用 GPU 设备
        if (!SDL_ClaimWindowForGPUDevice(m_gpuDevice, m_sdlWindow)) {
            Logger::Error("[ScreenOverlay] SDL_ClaimWindowForGPUDevice: {}", SDL_GetError());
            return false;
        }
        m_gpuWindowClaimed = true;
        SDL_SetGPUSwapchainParameters(m_gpuDevice, m_sdlWindow, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                      SDL_GPU_PRESENTMODE_VSYNC);
#if defined(SHOTLET_PLATFORM_MACOS)
        ApplyGpuBufferCount();
#endif
        return true;
    }

    void ScreenOverlay::DestroySDLWindow() {
        if (m_gpuDevice && m_sdlWindow && m_gpuWindowClaimed) {
            SDL_ReleaseWindowFromGPUDevice(m_gpuDevice, m_sdlWindow);
            m_gpuWindowClaimed = false;
        }
        m_gpuDevice = nullptr;
#if defined(SHOTLET_PLATFORM_LINUX)
        if (m_sdlWindow) {
            if (auto *renderer = SDL_GetRenderer(m_sdlWindow))
                SDL_DestroyRenderer(renderer);
        }
#endif
        if (m_sdlWindow) {
            SDL_DestroyWindow(m_sdlWindow);
            m_sdlWindow = nullptr;
        }
    }

    bool ScreenOverlay::InitializeImGui() {
        IMGUI_CHECKVERSION();
        m_imguiContext = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_imguiContext);

        ImGuiIO &io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.IniFilename = nullptr;

        const float uiScale = ClampedUIScale(m_config.scaleFactor);
        float fontPixelSize = GetTextLayerFontSize() * uiScale;
        ImFontConfig fontConfig;
        fontConfig.OversampleH = 1;
        fontConfig.OversampleV = 1;
        fontConfig.FontNo = GetChineseFontNo();
        fontConfig.FontDataOwnedByAtlas = false;

        ImFont *mainFont = nullptr;
        auto fontData = GetCachedFontData();
        auto fontDataSizes = GetCachedFontDataSizes();
        if (!fontData.empty()) {
            for (size_t i = 0; i < fontData.size(); i++) {
                mainFont = io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char *>(fontData[i]), fontDataSizes[i],
                                                          fontPixelSize, &fontConfig,
                                                          io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                if (mainFont)
                    break;
            }
        } else {
            auto fontPaths = GetChineseFontPaths();
            fontConfig.FontDataOwnedByAtlas = true;
            for (const auto &path: fontPaths) {
                mainFont = io.Fonts->AddFontFromFileTTF(path.c_str(), fontPixelSize, &fontConfig,
                                                        io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                if (mainFont)
                    break;
            }
        }
        if (!mainFont)
            mainFont = io.Fonts->AddFontDefault(&fontConfig);
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
                        const_cast<unsigned char *>(fontData[i]), fontDataSizes[i], tooltipFontPixelSize,
                        &tooltipFontConfig, io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                if (m_tooltipFont)
                    break;
            }
        } else {
            tooltipFontConfig.FontDataOwnedByAtlas = true;
            auto fontPaths = GetChineseFontPaths();
            for (const auto &path: fontPaths) {
                m_tooltipFont = io.Fonts->AddFontFromFileTTF(path.c_str(), tooltipFontPixelSize, &tooltipFontConfig,
                                                             io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
                if (m_tooltipFont)
                    break;
            }
        }

        ImGuiStyle &style = ImGui::GetStyle();
        style.WindowRounding = 11.0f;
        style.FrameRounding = 6.0f;
        style.GrabRounding = 6.0f;
        style.PopupRounding = 6.0f;
        style.ScrollbarRounding = 6.0f;
        style.TabRounding = 6.0f;
        ApplyImGuiTheme(style, m_config.theme);
        style.ScaleAllSizes(uiScale);

        bool platformReady = false;
#if defined(SHOTLET_PLATFORM_LINUX)
        if (!m_gpuDevice) {
            auto *renderer = SDL_GetRenderer(m_sdlWindow);
            platformReady = ImGui_ImplSDL3_InitForSDLRenderer(m_sdlWindow, renderer);
            if (platformReady && ImGui_ImplSDLRenderer3_Init(renderer))
                return true;
        } else
#endif
        {
            platformReady = ImGui_ImplSDL3_InitForOther(m_sdlWindow);
            ImGui_ImplSDLGPU3_InitInfo gpuInfo = {};
            gpuInfo.Device = m_gpuDevice;
            gpuInfo.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(m_gpuDevice, m_sdlWindow);
            gpuInfo.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
            gpuInfo.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
            gpuInfo.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
            if (platformReady && ImGui_ImplSDLGPU3_Init(&gpuInfo))
                return true;
        }
        if (platformReady)
            ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(m_imguiContext);
        m_imguiContext = nullptr;
        return false;
    }

    void ScreenOverlay::ShutdownImGui() {
        if (!m_imguiContext)
            return;
        ImGui::SetCurrentContext(m_imguiContext);
#if defined(SHOTLET_PLATFORM_LINUX)
        if (!m_gpuDevice)
            ImGui_ImplSDLRenderer3_Shutdown();
        else
#endif
            ImGui_ImplSDLGPU3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(m_imguiContext);
        m_imguiContext = nullptr;
    }

} // namespace shotlet
