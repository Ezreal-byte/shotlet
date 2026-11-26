#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <cstdlib>
#include <iterator>
#include "Application.h"
#include "capture/ScreenCapture.h"
#include "platform/ApplicationPlatform.h"
#include "platform/FontProvider.h"
#include "platform/HotkeyService.h"
#include "platform/PlatformMainContext.h"
#include "ui/OverlayManager.h"
#include "utils/Logger.h"

namespace shotlet {
    bool Application::Initialize(int argc, char **argv) {
        PlatformAutoreleasePool pool;
        // 宿主使用传入的主题、语言和日志路径，不读写独立应用的设置文件。
        if (!m_hostCaptureOutput)
            m_preferences = LoadPreferences(PreferencesPath());
        if (!InitializeLogging(argc, argv) || !InitializePreferences(argc, argv))
            return false;
        InitializeApplicationPlatform();
        if (!InitializeSDL()) {
            Logger::Error("Failed to initialize SDL");
            return false;
        }
        RefreshTheme(true);
        if (!InitializeScreenCapture())
            return false;

        // 预加载字体数据到内存
        auto fontData = GetCachedFontData();
        auto fontDataSizes = GetCachedFontDataSizes();
        Logger::Debug("Font data cached: {} file(s), total {} bytes", fontData.size(),
                      fontDataSizes.empty() ? 0 : fontDataSizes[0]);

        auto displays = m_screenCapture->GetDisplays();
        Logger::Debug("Found {} display(s)", displays.size());
        ParseGpuBufferArguments(argc, argv);
        InitializeHistory();
        if (m_daemonMode) {
            if (!InitializeDaemonMode())
                return false;
        } else {
            RequestCapture();
#if defined(SHOTLET_PLATFORM_LINUX)
            if (!m_capturing) {
                Logger::Error("Initial Linux capture failed; exiting instead of waiting without a window");
                return false;
            }
#endif
        }
        if (m_hostCaptureOutput && !m_capturing)
            return false;
        Logger::Info("Shotlet initialized successfully");
        return true;
    }

    bool Application::InitializeLogging(int argc, char **argv) {
        if (!m_preferences.logDirectory.empty())
            Logger::SetLogDirectoryOverride(m_preferences.logDirectory);
        // 先确定运行模式，初始化和退出日志都能明确是否收到后台参数。
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "-d" || arg == "--daemon") {
                m_daemonMode = true;
            }
        }
        // 宿主模式与后台、嵌入模式语义互斥。
        if (m_hostCaptureOutput && (m_daemonMode || m_embeddedMode))
            return false;
        // 日志参数先于平台初始化生效，初始化失败也能按配置输出。
        std::string logArgumentError;
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const bool consoleLevel = arg == "--console-log-level" || arg.starts_with("--console-log-level=");
            const bool logDirectory = arg == "--log-dir" || arg.starts_with("--log-dir=");
            if (arg == "--no-console-log") {
                Logger::SetConsoleEnabled(false);
            } else if (arg == "--log-level" || arg.starts_with("--log-level=") || consoleLevel || logDirectory) {
                std::string value;
                const auto equals = arg.find('=');
                if (equals != std::string::npos)
                    value = arg.substr(equals + 1);
                else if (i + 1 < argc && argv[i + 1][0] != '-')
                    value = argv[++i];
                if (logDirectory) {
                    if (value.empty())
                        logArgumentError = "--log-dir requires a non-empty directory";
                    else
                        Logger::SetLogDirectoryOverride(
                                std::filesystem::path(std::u8string(value.begin(), value.end())));
                } else {
                    LogLevel level;
                    if (!Logger::TryParseLevel(value, level))
                        logArgumentError = "Invalid log level '" + value + "'; expected debug, info, warn or error";
                    else if (consoleLevel)
                        Logger::SetConsoleLevel(level);
                    else
                        Logger::SetLevel(level);
                }
            }
        }
        const bool fileLogging = Logger::Init(m_embeddedMode);
        if (!logArgumentError.empty()) {
            Logger::Error("{}", logArgumentError);
            return false;
        }
        Logger::Info("event=app.started version={} platform={} embedded={} daemon={} argc={} file_logging={}",
                     SHOTLET_VERSION, SDL_GetPlatform(), m_embeddedMode, m_daemonMode, argc, fileLogging);
        return true;
    }

    bool Application::InitializePreferences(int argc, char **argv) {
        LanguagePreference preference = m_preferences.language;
        std::string languageError;
        if (!ParseLanguageArguments(argc, argv, preference, languageError)) {
            Logger::Error("{}", languageError);
            return false;
        }
        m_preferences.language = preference;
        m_language = ResolveLanguage(preference);
        m_themePreference = m_preferences.theme;

        std::string themeError;
        if (!ParseThemeArguments(argc, argv, m_themePreference, themeError)) {
            Logger::Error("{}", themeError);
            return false;
        }
        m_preferences.theme = m_themePreference;
        return true;
    }

    void Application::ParseGpuBufferArguments(int argc, char **argv) {
        for (int i = 1; i < argc; i++) {
            std::string arg = argv[i];
            if (arg == "--gpu-buffers" || arg.rfind("--gpu-buffers=", 0) == 0) {
                // swapchain drawable 数量：2=双倍缓冲（省 1/3 swapchain 内存，
                // 渲染超时更容易掉帧），3=系统默认三倍缓冲
                std::string value;
                if (arg == "--gpu-buffers") {
                    // 分离参数形式：仅当下一项不是 flag（不以 '-' 开头）时才消费，
                    // 避免缺少值时吞掉后续命令行选项
                    if (i + 1 < argc && argv[i + 1][0] != '-')
                        value = argv[++i];
                } else {
                    value = arg.substr(std::string("--gpu-buffers=").size());
                }
                const int bufferCount = value.empty() ? 0 : std::atoi(value.c_str());
                if (bufferCount == DoubleGpuBufferCount || bufferCount == TripleGpuBufferCount) {
                    m_gpuBufferCount = bufferCount;
                } else {
                    Logger::Warning("Invalid --gpu-buffers value '{}' (only {} or {}), using default {}", value,
                                    DoubleGpuBufferCount, TripleGpuBufferCount, DoubleGpuBufferCount);
                }
            }
        }
        Logger::Info("Config: gpu-buffers={}", m_gpuBufferCount);
    }

    bool Application::InitializeScreenCapture() {
        // 初始化屏幕捕获
        m_screenCapture = CreateScreenCapture();
        if (!m_screenCapture) {
            Logger::Error("Failed to create screen capture");
            return false;
        }

        if (!m_screenCapture->Initialize()) {
            Logger::Error("Failed to initialize screen capture: {}", m_screenCapture->GetLastErrorMessage());
            return false;
        }

        // 屏幕录制权限状态由宿主传入，授权引导由宿主负责，不进入独立应用的等待流程。
        if (!m_hostCaptureOutput)
            PrepareApplicationScreenCapture(m_screenCapture.get());
        return true;
    }

    bool Application::InitializeDaemonMode() {
        if (!m_systemTray.Create(m_language, m_trayTheme.value_or(Theme::Dark))) {
            Logger::Error("[Daemon] Failed to create system tray: {}", SDL_GetError());
            return false;
        }
        const auto bindings = m_preferences.Hotkeys();
        bool *registered[] = {&m_hotkeyRegistered, &m_historyHotkeyRegistered, &m_copyLatestHotkeyRegistered};
        for (std::size_t i = 0; i < std::size(bindings); ++i) {
            std::string conflict;
            if (bindings[i].IsEnabled()) {
                *registered[i] = HotkeyService::Register(bindings[i].modifiers, bindings[i].key, conflict, m_language,
                                                         HotkeyService::Action(i));
                if (!*registered[i]) {
                    Logger::Error("[Daemon] Hotkey registration failed: {}", conflict);
                    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, Tr(m_language, TextId::HotkeyErrorTitle),
                                             conflict.c_str(), nullptr);
                }
            }
        }
        // 预热窗口与字体；整屏图像延迟到用户触发时分配，降低启动内存峰值。
        OverlayManager::WarmupOverlays(m_screenCapture.get());
        Logger::Info("Shotlet daemon mode initialized");
        return true;
    }

    bool Application::InitializeSDL() {
        ApplyApplicationPlatformSDLHints();

        SDL_SetHint(SDL_HINT_IME_IMPLEMENTED_UI, "composition");

        if ((SDL_WasInit(SDL_INIT_VIDEO) & SDL_INIT_VIDEO) == 0) {
            if (!SDL_Init(SDL_INIT_VIDEO)) {
                Logger::Error("SDL_Init failed: {}", SDL_GetError());
                return false;
            }
            m_sdlInitializedByThisInstance = true;
        } else {
            Logger::Debug("SDL video subsystem already initialized");
        }

        return CreateGPUDevice();
    }

    bool Application::CreateGPUDevice() {
        if (m_gpuDevice) {
            return true;
        }

#if defined(SHOTLET_PLATFORM_LINUX)
        const char *renderer = SDL_getenv("SHOTLET_RENDERER");
        const std::string rendererMode = renderer ? renderer : "auto";
        if (rendererMode == "software") {
            Logger::Info("event=renderer.selected backend=software reason=requested");
            return true;
        }
        if (rendererMode != "auto" && rendererMode != "vulkan") {
            Logger::Error("Invalid SHOTLET_RENDERER '{}'; expected auto, software or vulkan", rendererMode);
            return false;
        }
#endif

        Logger::Debug("Creating SDL_GPU device...");
        SDL_GPUDevice *device = CreateApplicationGpuDevice(m_embeddedMode);
        if (!device) {
#if defined(SHOTLET_PLATFORM_LINUX)
            if (rendererMode == "auto") {
                Logger::Warning("Vulkan initialization failed: {}; using CPU software rendering", SDL_GetError());
                Logger::Info("event=renderer.selected backend=software reason=vulkan_unavailable");
                SDL_ClearError();
                return true;
            }
#endif
            Logger::Error("SDL_CreateGPUDevice failed: {}", SDL_GetError());
            return false;
        }
        m_gpuDevice = device;
        const char *driver = SDL_GetGPUDeviceDriver(device);
        Logger::Info("SDL_GPU device ready (driver: {})", driver ? driver : "unknown");
        return true;
    }
} // namespace shotlet
