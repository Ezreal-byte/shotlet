#include "OverlayManager.h"
#include "capture/ScreenCapture.h"
#include "core/Canvas.h"
#include "ui/ImageEditor.h"
#include "app/Application.h"
#include "platform/TransparentWindow.h"
#include "utils/Logger.h"
#include "utils/ImGuiUtils.h"

#include <SDL3/SDL.h>
#if defined(SHOTLET_PLATFORM_WINDOWS)
#include "platform/PlatformWin32.h"
#include "platform/WindowsCaptureGuard.h"
#endif
#include <cstring>
#include <future>
#include <chrono>
#include <algorithm>
#include <cmath>

namespace shotlet {

    namespace {
        bool ConfigMatches(const ScreenOverlay::Config& lhs,
                           const ScreenOverlay::Config& rhs) {
            return lhs.displayId == rhs.displayId &&
                   lhs.windowX == rhs.windowX &&
                   lhs.windowY == rhs.windowY &&
                   lhs.windowWidth == rhs.windowWidth &&
                   lhs.windowHeight == rhs.windowHeight &&
                   std::fabs(lhs.scaleFactor - rhs.scaleFactor) < 0.001f &&
                   lhs.physicalX == rhs.physicalX &&
                   lhs.physicalY == rhs.physicalY &&
                   lhs.physicalWidth == rhs.physicalWidth &&
                   lhs.physicalHeight == rhs.physicalHeight && lhs.language == rhs.language;
        }
    }

    static uint32_t GetMouseEventWindowID(const SDL_Event& event) {
        switch (event.type) {
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                return event.button.windowID;
            case SDL_EVENT_MOUSE_MOTION:
                return event.motion.windowID;
            default:
                return 0;
        }
    }

    OverlayManager::OverlayManager() = default;

    OverlayManager::~OverlayManager() {
        DestroyAll();
    }

    void OverlayManager::WarmupOverlays(IScreenCapture* capture) {
        if (!capture) return;

        auto t0 = std::chrono::high_resolution_clock::now();
        auto displays = capture->GetDisplays();
        if (displays.empty()) return;

        std::vector<std::unique_ptr<ScreenOverlay>> overlays;
        overlays.reserve(displays.size());
        for (const auto& display : displays) {
#if defined(SHOTLET_PLATFORM_WINDOWS)
            int windowWidth = display.displayWidth;
            int windowHeight = display.displayHeight;
#else
            int windowWidth = static_cast<int>(display.displayWidth / display.scaleFactor);
            int windowHeight = static_cast<int>(display.displayHeight / display.scaleFactor);
#endif

            ScreenOverlay::Config config;
            config.displayId = display.id;
            config.windowX = display.displayX;
            config.windowY = display.displayY;
            config.windowWidth = windowWidth;
            config.windowHeight = windowHeight;
            config.scaleFactor = display.scaleFactor;
            config.physicalX = display.displayX;
            config.physicalY = display.displayY;
            config.physicalWidth = display.displayWidth;
            config.physicalHeight = display.displayHeight;

            auto overlay = std::make_unique<ScreenOverlay>();
            if (overlay->PreCreate(config)) {
                overlays.push_back(std::move(overlay));
            }
        }

        for (auto& overlay : overlays) {
            overlay->Destroy();
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        Logger::Debug("[OverlayManager] WarmupOverlays: {} overlay(s) in {:.1f}ms",
                      overlays.size(),
                      std::chrono::duration<float, std::milli>(t1 - t0).count());
    }

    bool OverlayManager::CreateAll(IScreenCapture* capture, Language language, Theme theme) {
        if (!capture) {
            Logger::Error("[OverlayManager] No capture interface provided");
            return false;
        }
#if defined(SHOTLET_PLATFORM_WINDOWS)
        // CreateAll 也允许直接替换旧会话；后续几何变化可能销毁并重建 HWND。
        if (m_captureGuard) HideAll();
#endif

        auto t0 = std::chrono::high_resolution_clock::now();

        // 获取所有显示器信息
        auto displays = capture->GetDisplays();
        if (displays.empty()) {
            Logger::Error("[OverlayManager] No displays found");
            return false;
        }

        for (const auto& display : displays) {
            Logger::Debug("[OverlayManager] Display[{}]: name='{}', pos=({},{}), size={}x{}, scale={:.2f}, primary={}",
                          display.id, display.name, display.displayX, display.displayY,
                          display.displayWidth, display.displayHeight, display.scaleFactor, display.isPrimary);
        }

        // 获取当前鼠标位置，确定初始活跃屏幕
        float mouseX, mouseY;
        SDL_GetGlobalMouseState(&mouseX, &mouseY);
        int initialDisplayId = -1;

        // 确定初始活跃屏幕
        for (const auto& display : displays) {
#if defined(SHOTLET_PLATFORM_WINDOWS)
            // Windows V2: mouse coords and display coords are both physical pixels
            int hitWidth = display.displayWidth;
            int hitHeight = display.displayHeight;
#else
            // macOS: mouse coords are logical points, displayWidth is physical pixels
            int hitWidth = static_cast<int>(display.displayWidth / display.scaleFactor);
            int hitHeight = static_cast<int>(display.displayHeight / display.scaleFactor);
#endif
            if (initialDisplayId < 0 &&
                mouseX >= display.displayX && mouseX < display.displayX + hitWidth &&
                mouseY >= display.displayY && mouseY < display.displayY + hitHeight) {
                initialDisplayId = display.id;
            }
        }

        // === Phase A: 并行截图 + PreCreate overlays（重叠执行）===

        auto t_a_start = std::chrono::high_resolution_clock::now();

        // 启动截图。后端必须显式声明 Capture() 支持同实例并发调用，
        // 否则串行捕获，避免平台缓存和错误状态出现数据竞争。
        std::vector<std::future<CaptureImage>> captureFutures;
        std::vector<CaptureImage> sequentialCaptures;
        auto makeOptions = [](const DisplayInfo& display) {
            CaptureOptions options;
            options.displayId = display.id;
            options.hasDisplayInfo = true;
            options.displayInfo = display;
            return options;
        };

        if (capture->SupportsConcurrentCapture()) {
            for (const auto& display : displays) {
                CaptureOptions options = makeOptions(display);
                captureFutures.push_back(std::async(std::launch::async, [capture, options]() {
                    return capture->Capture(options);
                }));
            }
        } else {
            sequentialCaptures.reserve(displays.size());
            for (const auto& display : displays) {
                sequentialCaptures.push_back(capture->Capture(makeOptions(display)));
            }
        }

        auto t_capture_launch = std::chrono::high_resolution_clock::now();

        // PreCreate 所有 overlay（主线程，与截图并行执行）
        if (!m_overlays.empty()) {
            for (auto& overlay : m_overlays) {
                overlay->ResetForReuse();
            }
            std::erase_if(m_overlays, [&displays](const auto& overlay) {
                const bool stillPresent = std::any_of(displays.begin(), displays.end(),
                    [&overlay](const DisplayInfo& display) {
                        return display.id == overlay->GetDisplayId();
                    });
                if (!stillPresent) {
                    Logger::Debug("[OverlayManager] Remove stale overlay for display {}",
                                  overlay->GetDisplayId());
                    overlay->Destroy();
                }
                return !stillPresent;
            });
        }

        for (const auto& display : displays) {
#if defined(SHOTLET_PLATFORM_WINDOWS)
            int windowWidth = display.displayWidth;
            int windowHeight = display.displayHeight;
#else
            int windowWidth = static_cast<int>(display.displayWidth / display.scaleFactor);
            int windowHeight = static_cast<int>(display.displayHeight / display.scaleFactor);
#endif

            ScreenOverlay::Config config;
            config.displayId = display.id;
            config.windowX = display.displayX;
            config.windowY = display.displayY;
            config.windowWidth = windowWidth;
            config.windowHeight = windowHeight;
            config.scaleFactor = display.scaleFactor;
            config.physicalX = display.displayX;
            config.physicalY = display.displayY;
            config.physicalWidth = display.displayWidth;
            config.physicalHeight = display.displayHeight;

            config.language = language;
            config.theme = theme;

            ScreenOverlay* existing = GetOverlay(display.id);
            if (existing && existing->IsPreCreated()) {
                if (ConfigMatches(existing->GetConfig(), config)) {
                    existing->SetTheme(theme);
                    continue;
                }

                Logger::Debug("[OverlayManager] Recreate overlay for display {} after geometry/scale change",
                              display.id);
                existing->Destroy();
                std::erase_if(m_overlays, [displayId = display.id](const auto& overlay) {
                    return overlay->GetDisplayId() == displayId;
                });
            }

            auto overlay = std::make_unique<ScreenOverlay>();
            if (!overlay->PreCreate(config)) {
                Logger::Error("[OverlayManager] Failed to pre-create overlay for display {}", display.id);
                continue;
            }
#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)
            // 注入滚动截图完成回调：拼接结果上交给 manager 创建编辑器（打破反向依赖）
            auto* source = overlay.get();
            overlay->SetScrollImageCallback([this, source](std::shared_ptr<const ScrollImage> image) {
                return HandleScrollImage(image, source);
            });
#endif
            m_overlays.push_back(std::move(overlay));
        }

        auto t1 = std::chrono::high_resolution_clock::now();
        Logger::Debug("[OverlayManager] Phase A detail: GetDisplays+mouse={:.1f}ms, "
                      "CaptureLaunch={:.1f}ms, PreCreate overlays={:.1f}ms, Phase A total={:.1f}ms",
                      std::chrono::duration<float, std::milli>(t_a_start - t0).count(),
                      std::chrono::duration<float, std::milli>(t_capture_launch - t_a_start).count(),
                      std::chrono::duration<float, std::milli>(t1 - t_capture_launch).count(),
                      std::chrono::duration<float, std::milli>(t1 - t0).count());

        // === Phase B: 等待截图完成 ===

        std::vector<CaptureImage> captureImages;
        std::vector<ScreenData> screenDataList;
        float waitFuturesMs = 0.0f;
        auto appendCapturedImage = [&](CaptureImage img, size_t i, float futureWaitMs) {
            if (!img.data) {
                Logger::Warning("[OverlayManager] Failed to capture display {} (future wait {:.1f}ms)",
                                i, futureWaitMs);
                return;
            }

            Logger::Debug("[OverlayManager] Capture[{}]: {}x{}, displayId={}, rowPitch={}, data={:.1f}MB, futureWait={:.1f}ms",
                          i, img.width, img.height, img.displayId, img.rowPitch,
                          static_cast<double>(img.GetDataSize()) / (1024.0 * 1024.0),
                          futureWaitMs);

            captureImages.push_back(std::move(img));
        };

        for (size_t i = 0; i < sequentialCaptures.size(); i++) {
            appendCapturedImage(std::move(sequentialCaptures[i]), i, 0.0f);
        }

        for (size_t i = 0; i < captureFutures.size(); i++) {
            auto t_future_wait_start = std::chrono::high_resolution_clock::now();
            auto img = captureFutures[i].get();
            auto t_future_wait_end = std::chrono::high_resolution_clock::now();
            float futureWaitMs = std::chrono::duration<float, std::milli>(
                t_future_wait_end - t_future_wait_start).count();
            waitFuturesMs += futureWaitMs;
            appendCapturedImage(std::move(img), i, futureWaitMs);
        }

        auto t2 = std::chrono::high_resolution_clock::now();
        Logger::Debug("[OverlayManager] Phase B detail: FutureWaitTotal={:.1f}ms",
                      waitFuturesMs);
        Logger::Debug("[OverlayManager] Phase B (Capture complete): {:.1f}ms",
                      std::chrono::duration<float, std::milli>(t2 - t0).count());

        if (captureImages.empty()) {
            Logger::Error("[OverlayManager] No screens captured");
            return false;
        }

        if (m_overlays.empty()) {
            Logger::Error("[OverlayManager] No overlays pre-created");
            return false;
        }

        // === Phase C1: 设置底图数据 + 立即显示背景（用户最快看到截图）===

        for (size_t i = 0; i < captureImages.size(); i++) {
            const auto& img = captureImages[i];

            for (auto& overlay : m_overlays) {
                if (overlay->GetDisplayId() == img.displayId) {
                    auto t_bg_start = std::chrono::high_resolution_clock::now();
                    overlay->UpdateNativeBackground(img.data, img.width, img.height, img.rowPitch);
                    auto t_bg_update = std::chrono::high_resolution_clock::now();
                    overlay->ShowBackground();
                    auto t_bg_show = std::chrono::high_resolution_clock::now();
                    Logger::Debug("[OverlayManager] Native background[{}]: Update={:.1f}ms, Show={:.1f}ms, total={:.1f}ms",
                                  img.displayId,
                                  std::chrono::duration<float, std::milli>(t_bg_update - t_bg_start).count(),
                                  std::chrono::duration<float, std::milli>(t_bg_show - t_bg_update).count(),
                                  std::chrono::duration<float, std::milli>(t_bg_show - t_bg_start).count());
                    break;
                }
            }
        }

        auto t_c1 = std::chrono::high_resolution_clock::now();
        Logger::Debug("[OverlayManager] Phase C1 (Background visible): {:.1f}ms",
                      std::chrono::duration<float, std::milli>(t_c1 - t0).count());

        // === Phase C2: 设置 GPU 纹理 + Canvas ===

        bool anySetCaptureDataFailed = false;
        for (size_t i = 0; i < captureImages.size(); i++) {
            auto& img = captureImages[i];
            for (auto& overlay : m_overlays) {
                if (overlay->GetDisplayId() == img.displayId) {
                    auto t_set_start = std::chrono::high_resolution_clock::now();
#if defined(SHOTLET_PLATFORM_WINDOWS)
                    const size_t dataSize = static_cast<size_t>(img.rowPitch) * static_cast<size_t>(img.height);
                    // CreateDIBSection 的内存和 HBITMAP 关联，DeleteObject 会释放底层数据。
                    // 必须拷贝到独立分配的内存，否则 CaptureImage 析构时 DeleteObject 释放 GDI 内存后
                    // Canvas 持有的指针会指向已释放的内存，导致访问崩溃。
                    std::unique_ptr<unsigned char[]> canvasCopy;
                    std::unique_ptr<uint8_t[]> magnifierCopy;
                    if (img.data && dataSize > 0) {
                        canvasCopy = std::make_unique<unsigned char[]>(dataSize);
                        std::memcpy(canvasCopy.get(), img.data, dataSize);
                        magnifierCopy = std::make_unique<uint8_t[]>(dataSize);
                        std::memcpy(magnifierCopy.get(), img.data, dataSize);
                    }
#else
                    const uint8_t* borrowedImageData = img.data;
#endif
                    std::unique_ptr<unsigned char[]> ownedImageData;
#if defined(SHOTLET_PLATFORM_WINDOWS)
                    ownedImageData = std::move(canvasCopy);
#else
                    ownedImageData.reset(img.data);
#endif
                    img.data = nullptr;
                    if (!overlay->SetCaptureData(std::move(ownedImageData), img.width, img.height, img.rowPitch)) {
                        Logger::Error("[OverlayManager] Failed to set capture data for display {}", img.displayId);
                        anySetCaptureDataFailed = true;
                    } else {
                        ScreenData sd;
                        sd.pixelWidth = img.width;
                        sd.pixelHeight = img.height;
                        sd.screenX = img.captureX;
                        sd.screenY = img.captureY;
                        sd.rowPitch = img.rowPitch;
                        sd.scaleFactor = img.scaleFactor;
                        sd.displayId = img.displayId;
#if defined(SHOTLET_PLATFORM_WINDOWS)
                        sd.data = std::move(magnifierCopy);
#else
                        sd.borrowedData = borrowedImageData;
#endif
                        screenDataList.push_back(std::move(sd));
                    }
                    auto t_set_end = std::chrono::high_resolution_clock::now();
                    Logger::Debug("[OverlayManager] SetCaptureData[{}] total={:.1f}ms",
                                  img.displayId,
                                  std::chrono::duration<float, std::milli>(t_set_end - t_set_start).count());
                    break;
                }
            }
        }

        auto t_c2 = std::chrono::high_resolution_clock::now();

        // SetCaptureData 失败的 overlay 其 m_canvas 为空（ScreenOverlay 仅在成功时才创建 Canvas），
        // 若留在 m_overlays 里继续走 ProcessFrame，会在无判空的循环里解引用空指针。
        // 整个会话标记失败，交给调用方（StartCapture）的清理路径处理。
        if (anySetCaptureDataFailed) {
            Logger::Error("[OverlayManager] SetCaptureData failed, aborting capture session");
            return false;
        }

#if defined(SHOTLET_PLATFORM_WINDOWS)
        if (!StartCaptureGuard()) return false;
#endif

        // ShowOverlay + RenderFrame（主线程，需要窗口操作）
        for (auto& overlay : m_overlays) {
#if defined(SHOTLET_PLATFORM_WINDOWS)
            if (!UpdateCaptureGuard()) return false;
#endif
            if (overlay->IsCreated()) {
                overlay->ShowOverlay();
            }
        }

        auto t3 = std::chrono::high_resolution_clock::now();
        Logger::Debug("[OverlayManager] Phase C detail: SetCaptureData={:.1f}ms, ShowOverlay={:.1f}ms, total={:.1f}ms",
                      std::chrono::duration<float, std::milli>(t_c2 - t_c1).count(),
                      std::chrono::duration<float, std::milli>(t3 - t_c2).count(),
                      std::chrono::duration<float, std::milli>(t3 - t0).count());

        // 检查是否有 overlay 成功创建并显示
        bool anyVisible = false;
        for (auto& overlay : m_overlays) {
            if (overlay->IsCreated()) {
                anyVisible = true;
                break;
            }
        }
        if (!anyVisible) {
            Logger::Error("[OverlayManager] No overlays created");
            return false;
        }

        // 设置初始活跃屏幕
        if (initialDisplayId < 0) {
            initialDisplayId = m_overlays[0]->GetDisplayId();
        }
        m_activeDisplayId = initialDisplayId;
        m_initialMousePos = ImVec2(-1, -1);
        m_mouseHasMoved = false;

        // 设置活跃状态：先 Stop 非活跃窗口，再 Start 活跃窗口
        for (auto& overlay : m_overlays) {
            if (overlay->GetDisplayId() != m_activeDisplayId) {
                overlay->SetActive(false);
            }
        }
        for (auto& overlay : m_overlays) {
            if (overlay->GetDisplayId() == m_activeDisplayId) {
                overlay->SetActive(true);
            }
        }

        // 创建放大镜并设置多屏数据
        m_magnifier = std::make_unique<MagnifierOverlay>();
        m_magnifier->SetLanguage(language);
        m_magnifier->SetTheme(theme);
        float magnifierUIScale = 1.0f;
        for (const auto& display : displays) {
            if (display.id == initialDisplayId) {
                magnifierUIScale = display.scaleFactor;
                break;
            }
        }
        m_magnifier->SetUIScale(magnifierUIScale);
        if (m_magnifier->Initialize()) {
            m_magnifier->SetScreenData(std::move(screenDataList));
            m_magnifier->Show();
        }

        // 激活初始活跃窗口：延迟到首帧 ProcessFrame 中执行，
        // 因为 CreateAll 期间 Cocoa 事件循环尚未运转，NSApp 无法激活
        m_forceActivatePending = true;

        Logger::Debug("[OverlayManager] CreateAll total: {:.1f}ms",
                      std::chrono::duration<float, std::milli>(t3 - t0).count());

        return true;
    }

    void OverlayManager::DestroyAll() {
        HideAll();
        if (m_overlays.empty()) return;

        // 编辑器先于 overlay 销毁（其 GPU 资源等待与释放自成体系）
        if (m_imageEditor) m_imageEditor.reset();

        // 隐藏并清理放大镜
        if (m_magnifier) {
            m_magnifier->Hide();
            m_magnifier->ClearScreenData();
            m_magnifier->Shutdown();
            m_magnifier.reset();
        }

        for (auto& overlay : m_overlays) {
            overlay->Destroy();
        }
        m_overlays.clear();
        m_activeDisplayId = -1;

        // 截图结束，恢复 Accessory 模式隐藏 Dock 图标
        RestoreActivationPolicy();
    }

    void OverlayManager::HideAll() {
        for (auto& overlay : m_overlays) overlay->Hide();
        if (m_magnifier) m_magnifier->Hide();
        if (m_imageEditor && m_imageEditor->GetSDLWindow()) {
            SDL_HideWindow(m_imageEditor->GetSDLWindow());
        }
#if defined(SHOTLET_PLATFORM_WINDOWS)
        // 先隐藏再停止保护；窗口句柄只能在保护线程退出后销毁或复用。
        m_captureGuard.reset();
#endif
    }

    void OverlayManager::ResetForReuse() {
        HideAll();
        if (m_imageEditor) m_imageEditor.reset();

        if (m_magnifier) {
            m_magnifier->Hide();
            m_magnifier->ClearScreenData();
            m_magnifier->Shutdown();
            m_magnifier.reset();
        }

        for (auto& overlay : m_overlays) {
            overlay->ResetForReuse();
        }
        m_activeDisplayId = -1;
        m_initialMousePos = ImVec2(-1, -1);
        m_mouseHasMoved = false;
        m_forceActivatePending = false;

        RestoreActivationPolicy();
    }

    void OverlayManager::EnterImageEditor() {
        for (auto& overlay : m_overlays) {
            overlay->Hide();
            overlay->SetActive(false);
        }
        if (m_magnifier) m_magnifier->Hide();
#if defined(SHOTLET_PLATFORM_WINDOWS)
        m_captureGuard.reset();
#endif
        m_activeDisplayId = m_imageEditor->GetConfig().displayId;
    }

#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)
    bool OverlayManager::HandleScrollImage(std::shared_ptr<const ScrollImage> image, ScreenOverlay* source) {
        if (m_imageEditor) return false;  // 防御：同一会话不应出现第二个编辑器
        m_imageEditor = std::make_unique<ImageEditor>(source->GetConfig(), image,
                                                      Application::Instance()->GetGPUDevice());
        if (!m_imageEditor->Create()) {
            m_imageEditor.reset();
            return false;
        }
        // 仅创建成功才隐藏 overlay（对应旧路径：ApplyScrollImage 失败时 overlay 可见并弹提示）
        EnterImageEditor();
        // 同帧首渲染，窗口显示后立即有内容（NSWindow 背景色即 0.17 灰，规避白闪）
        m_imageEditor->RenderFrame();
        return true;
    }
#endif

    void OverlayManager::SetTheme(Theme theme) {
        for (auto& overlay : m_overlays) overlay->SetTheme(theme);
        if (m_imageEditor) m_imageEditor->SetTheme(theme);
        if (m_magnifier) m_magnifier->SetTheme(theme);
    }

    bool OverlayManager::OpenDocument(std::shared_ptr<const CaptureDocument> document,
                                      const ScreenOverlay::Config& config, SDL_GPUDevice* device) {
#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)
        auto editor = std::make_unique<ImageEditor>(std::move(document), config, device);
        if (!editor->Create()) return false;
        DestroyAll(); m_imageEditor = std::move(editor); return true;
#else
        (void)document; (void)config; (void)device; return false;
#endif
    }

#if defined(SHOTLET_PLATFORM_WINDOWS)
    bool OverlayManager::StartCaptureGuard() {
        // 必须先建立救援线程再显示首屏，避免多屏首帧或 GPU 提交卡住后留下全屏遮罩。
        std::vector<HWND> windows;
        for (const auto& overlay : m_overlays) {
            if (!overlay->IsCreated()) continue;
            auto props = SDL_GetWindowProperties(overlay->GetSDLWindow());
            auto hwnd = static_cast<HWND>(SDL_GetPointerProperty(
                props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
            if (!hwnd) {
                Logger::Error("[OverlayManager] Cannot guard overlay without native window");
                return false;
            }
            windows.push_back(hwnd);
        }
        m_captureGuard = std::make_unique<WindowsCaptureGuard>();
        if (!m_captureGuard->Start(std::move(windows))) {
            Logger::Error("[OverlayManager] Cannot start capture safety guard");
            return false;
        }
        m_winEscWasDown = false;
        m_winCtrlCWasDown = false;
        return true;
    }

    bool OverlayManager::UpdateCaptureGuard() {
        if (!m_captureGuard) return true;
        const auto reason = m_captureGuard->GetReason();
        if (reason == WindowsCaptureGuard::Reason::None) {
            const bool scrolling = std::any_of(m_overlays.begin(), m_overlays.end(),
                [](const auto& overlay) { return overlay->IsScrollCapturePendingOrActive(); });
            m_captureGuard->SetForegroundRequired(!scrolling);
            m_captureGuard->Heartbeat();
            return true;
        }
        for (auto& overlay : m_overlays) overlay->RequestClose();
        HideAll();
        Logger::Warning("[OverlayManager] Capture safety guard cancelled session: {}",
                        CaptureSafetyState::ReasonName(reason));
        return false;
    }
#endif

    void OverlayManager::ProcessFrame() {
        if (m_imageEditor) {
            ImageEditor* editor = m_imageEditor.get();
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (m_externalEvent && m_externalEvent(event)) continue;
            if (event.type == SDL_EVENT_SYSTEM_THEME_CHANGED) {
                if (m_themeChanged) m_themeChanged();
                continue;
            }
                if (event.type == SDL_EVENT_QUIT) { editor->RequestClose(); continue; }
                SDL_Window* sourceWindow = SDL_GetWindowFromEvent(&event);
                if (!sourceWindow) continue;
                const uint32_t id = SDL_GetWindowID(sourceWindow);
                if (!editor->HasWindowID(id)) continue;
                if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                    if (editor->IsCropping()) editor->CancelCrop();
                    else if (ImGuiUtils::HasActiveTextInput(editor->GetImGuiContext())) editor->ProcessEvent(event);
                    else if (editor->GetCanvas()->IsTextInputActive()) editor->GetCanvas()->CancelTextInput();
                    else editor->RequestClose();
                } else editor->ProcessEvent(event);
            }
            if (!editor->ShouldClose()) editor->RenderFrame();
            return;
        }

        if (m_overlays.empty()) return;
#if defined(SHOTLET_PLATFORM_WINDOWS)
        if (!UpdateCaptureGuard()) return;
#endif
        // 首帧取得输入焦点，避免 Windows 游戏继续占有输入；
        // macOS 的 Cocoa 事件循环此时也已运转，可以完成 NSApp 激活。
        if (m_forceActivatePending) {
            m_forceActivatePending = false;
            if (auto* active = GetActiveOverlay()) {
                active->ForceActivate();
            } else {
                Logger::Warning("[OverlayManager] ForceActivate skipped: no active overlay");
            }
        }

        // 本帧"是否有选区"只在事件处理前计算一次（事件后状态由放大镜段自行计算）
        bool anyHasSelection = false;
        for (auto& overlay : m_overlays) {
            if (overlay->GetCanvas() && overlay->GetCanvas()->HasSelection()) {
                anyHasSelection = true;
                break;
            }
        }

        // 更新活跃屏幕
        UpdateActiveScreen(anyHasSelection);
        SyncActiveOverlayToSelection();

#if defined(SHOTLET_PLATFORM_WINDOWS)
        {
            POINT pt;
            GetCursorPos(&pt);
            bool leftDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
            ScreenOverlay* mouseOverlay = HitTestMouseOverlay(static_cast<float>(pt.x), static_cast<float>(pt.y));
            if (!mouseOverlay) {
                mouseOverlay = GetActiveOverlay();
            }
            if (mouseOverlay) {
                mouseOverlay->ProcessGlobalMouseState(pt.x, pt.y, leftDown);
            }
        }
#endif

        // 设置选区锁定：已有选区时，其他屏幕不允许画新选区
        for (auto& overlay : m_overlays) {
            bool locked = anyHasSelection && overlay->GetCanvas() && !overlay->GetCanvas()->HasSelection();
            overlay->SetSelectionLocked(locked);
        }

        // 集中处理所有 SDL 事件，分发给对应的 Overlay
#if defined(SHOTLET_PLATFORM_WINDOWS)
        {
            bool escDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
            if (escDown && !m_winEscWasDown) {
                const auto* active = GetActiveOverlay();
                // 输入框持有焦点时由下面的 SDL 键盘事件结束编辑，不能被全局轮询提前关闭。
                if (!active || !ImGuiUtils::HasActiveTextInput(active->GetImGuiContext())) {
                    for (auto& overlay : m_overlays) overlay->RequestClose();
                }
            }
            m_winEscWasDown = escDown;

            // Ctrl+C 复制放大镜色值到剪贴板
            bool ctrlDown = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            bool cDown = (GetAsyncKeyState('C') & 0x8000) != 0;
            bool ctrlCDown = ctrlDown && cDown;
            if (ctrlCDown && !m_winCtrlCWasDown && m_magnifier && m_magnifier->IsVisible()) {
                m_magnifier->CopyColorToClipboard();
            }
            m_winCtrlCWasDown = ctrlCDown;
        }
#endif
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
                if (m_externalEvent && m_externalEvent(event)) continue;
            if (event.type == SDL_EVENT_SYSTEM_THEME_CHANGED) {
                if (m_themeChanged) m_themeChanged();
                continue;
            }
            if (event.type == SDL_EVENT_QUIT) {
                // 退出所有 Overlay
                for (auto& overlay : m_overlays) {
                    overlay->RequestClose();
                }
                continue;
            }

            if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_ESCAPE) {
                auto* active = GetActiveOverlay();
                if (active && ImGuiUtils::HasActiveTextInput(active->GetImGuiContext())) {
                    active->ProcessEvent(event);
                    continue;
                }
                // ESC 取消截图
                for (auto& overlay : m_overlays) {
                    overlay->RequestClose();
                }
                continue;
            }

            // 根据事件中的 windowID 找到对应的 Overlay
            if (event.type == SDL_EVENT_WINDOW_MOUSE_ENTER ||
                event.type == SDL_EVENT_WINDOW_MOUSE_LEAVE ||
                event.type == SDL_EVENT_WINDOW_FOCUS_GAINED ||
                event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                // 窗口事件，分发给对应 Overlay
                for (auto& overlay : m_overlays) {
                    if (overlay->HasWindowID(event.window.windowID)) {
                        overlay->ProcessEvent(event);
                        break;
                    }
                }
            } else if (event.type == SDL_EVENT_TEXT_INPUT || event.type == SDL_EVENT_TEXT_EDITING) {
                // IME 事件只分发给活跃 Overlay
                ScreenOverlay* activeOverlay = GetActiveOverlay();
                if (activeOverlay) {
                    activeOverlay->ProcessEvent(event);
                }
            } else {
                // 其他事件（鼠标、键盘等）分发给活跃 Overlay
                ScreenOverlay* activeOverlay = nullptr;
                if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                    event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
                    event.type == SDL_EVENT_MOUSE_MOTION) {
                    uint32_t windowID = GetMouseEventWindowID(event);
                    for (auto& overlay : m_overlays) {
                        if (overlay->HasWindowID(windowID)) {
                            bool overlayHasSelection = overlay->GetCanvas() && overlay->GetCanvas()->HasSelection();
                            if (anyHasSelection && !overlayHasSelection) {
                                break;
                            }
                            activeOverlay = overlay.get();
                            ActivateOverlay(activeOverlay, false);
                            break;
                        }
                    }
                }
                if (!activeOverlay) {
                    activeOverlay = GetActiveOverlay();
                }
                if (activeOverlay) {
                    activeOverlay->ProcessEvent(event);
                }
            }
        }

        // 取消后立即隐藏，不再提交最后一帧，避免退出时出现额外的遮罩或黑帧。
        if (IsCancelled()) {
            HideAll();
            return;
        }
        SyncActiveOverlayToSelection();

        // 检测选区状态，控制放大镜显示/隐藏
        // 放大镜只在鼠标位于没有选区的屏幕时显示
        if (m_magnifier) {
            // 事件处理后的选区状态（事件中可能新建选区，不能用事件前的 anyHasSelection）
            bool postEventHasSelection = false;
            for (auto& overlay : m_overlays) {
                if (overlay->GetCanvas() && overlay->GetCanvas()->HasSelection()) {
                    postEventHasSelection = true;
                    break;
                }
            }

            float mouseX, mouseY;
            SDL_GetGlobalMouseState(&mouseX, &mouseY);
            m_magnifier->RefreshMousePosition();

            ScreenOverlay* mouseOverlay = HitTestMouseOverlay(mouseX, mouseY);

            bool shouldShow = true;
            if (postEventHasSelection) {
                // 有选区时：只在鼠标不在有选区的屏幕上才显示放大镜
                if (mouseOverlay && mouseOverlay->GetCanvas() && mouseOverlay->GetCanvas()->HasSelection()) {
                    shouldShow = false;
                }
            }

            if (shouldShow && !m_magnifier->IsVisible()) {
                m_magnifier->Show();
            } else if (!shouldShow && m_magnifier->IsVisible()) {
                m_magnifier->Hide();
            }
        }

        // 渲染所有 Overlay（编辑器激活时上方分支已提前 return，不会走到这里）
        for (auto& overlay : m_overlays) {
            overlay->RenderFrame();
        }
    }

    void OverlayManager::ActivateOverlay(ScreenOverlay* overlay, bool forceFocus) {
        if (!overlay) return;

        bool overlayHasSelection = overlay->GetCanvas() && overlay->GetCanvas()->HasSelection();
        bool shouldRemoveDim = m_mouseHasMoved || overlayHasSelection;

        int newActiveId = overlay->GetDisplayId();
        if (newActiveId == m_activeDisplayId && overlay->IsActive()) {
            if (shouldRemoveDim && overlay->IsDimmed()) {
                overlay->RemoveDim();
            }
            if (forceFocus) {
                overlay->ForceActivate();
            }
            return;
        }

        int oldId = m_activeDisplayId;
        m_activeDisplayId = newActiveId;

        for (auto& o : m_overlays) {
            if (o.get() != overlay) {
                o->SetActive(false);
            }
        }
        overlay->SetActive(true);
        if (shouldRemoveDim) {
            overlay->RemoveDim();
        }

        Logger::Debug("[OverlayManager] Active screen changed: {} -> {}", oldId, m_activeDisplayId);

        if (forceFocus) {
            overlay->ForceActivate();
        }
    }

    void OverlayManager::SyncActiveOverlayToSelection() {
        for (auto& overlay : m_overlays) {
            if (overlay->GetCanvas() && overlay->GetCanvas()->HasSelection()) {
                ActivateOverlay(overlay.get(), false);
                return;
            }
        }
    }

    ScreenOverlay* OverlayManager::GetActiveOverlay() const {
        for (auto& overlay : m_overlays) {
            if (overlay->IsActive()) {
                return overlay.get();
            }
        }
        return nullptr;
    }

    ScreenOverlay* OverlayManager::GetOverlay(int displayId) const {
        for (auto& overlay : m_overlays) {
            if (overlay->GetDisplayId() == displayId) {
                return overlay.get();
            }
        }
        return nullptr;
    }

    bool OverlayManager::IsFinished() const {
        if (m_imageEditor) return m_imageEditor->HasResult();
        ScreenOverlay* active = GetActiveOverlay();
        return active && active->HasResult();
    }

    bool OverlayManager::IsCancelled() const {
        if (m_imageEditor) return m_imageEditor->ShouldClose();
        ScreenOverlay* active = GetActiveOverlay();
        return active && active->ShouldClose() && !active->HasResult();
    }

    bool OverlayManager::IsPinRequested() const {
        if (m_imageEditor) return m_imageEditor->IsPinRequested();
        for (const auto& overlay : m_overlays) {
            if (overlay->IsPinRequested()) {
                return true;
            }
        }
        return false;
    }

    bool OverlayManager::IsSaveRequested() const {
        if (m_imageEditor) return m_imageEditor->IsSaveRequested();
        for (const auto& overlay : m_overlays) {
            if (overlay->IsSaveRequested()) {
                return true;
            }
        }
        return false;
    }

    bool OverlayManager::IsRecordRequested() const {
        if (m_imageEditor) return false;
        const auto* overlay = GetActiveOverlay();
        return overlay && overlay->IsRecordRequested();
    }

    bool OverlayManager::IsOcrRequested() const {
        if (m_imageEditor) return m_imageEditor->IsOcrRequested();
        for (const auto& overlay : m_overlays) {
            if (overlay->IsOcrRequested()) {
                return true;
            }
        }
        return false;
    }

    // —— 结果导出上下文：编辑器独占时路由到编辑器，否则取活跃 overlay ——

    Canvas* OverlayManager::GetResultCanvas() const {
        if (m_imageEditor) return m_imageEditor->GetCanvas();
        ScreenOverlay* active = GetActiveOverlay();
        return active ? active->GetCanvas() : nullptr;
    }

    SDL_Window* OverlayManager::GetResultSDLWindow() const {
        if (m_imageEditor) return m_imageEditor->GetSDLWindow();
        ScreenOverlay* active = GetActiveOverlay();
        return active ? active->GetSDLWindow() : nullptr;
    }

    ImGuiContext* OverlayManager::GetResultImGuiContext() const {
        if (m_imageEditor) return m_imageEditor->GetImGuiContext();
        ScreenOverlay* active = GetActiveOverlay();
        return active ? active->GetImGuiContext() : nullptr;
    }

    const ScreenOverlay::Config* OverlayManager::GetResultConfig() const {
        if (m_imageEditor) return &m_imageEditor->GetConfig();
        ScreenOverlay* active = GetActiveOverlay();
        return active ? &active->GetConfig() : nullptr;
    }

    ImVec2 OverlayManager::GetResultSelectionScreenOrigin() const {
        // 编辑器路径：选区为图片坐标，交给编辑器做视图变换（见 EditorSelectionScreenAnchor）
        if (m_imageEditor) return m_imageEditor->GetSelectionScreenOrigin();
        // overlay 路径：全屏 overlay 的选区即屏幕坐标，窗口原点 + 选区
        ScreenOverlay* active = GetActiveOverlay();
        if (active && active->GetCanvas()) {
            const ImRect selection = active->GetCanvas()->GetSelection();
            const auto& config = active->GetConfig();
            return ImVec2(config.windowX + selection.Min.x, config.windowY + selection.Min.y);
        }
        return ImVec2(-1, -1);
    }

    ScreenOverlay* OverlayManager::HitTestMouseOverlay(float mouseX, float mouseY) const {
        for (const auto& overlay : m_overlays) {
            const auto& config = overlay->GetConfig();
#if defined(SHOTLET_PLATFORM_WINDOWS)
            // Windows Per-Monitor V2: mouse coords are physical pixels
            if (mouseX >= config.physicalX && mouseX < config.physicalX + config.physicalWidth &&
                mouseY >= config.physicalY && mouseY < config.physicalY + config.physicalHeight) {
#else
            // macOS: mouse coords are logical points
            if (mouseX >= config.windowX && mouseX < config.windowX + config.windowWidth &&
                mouseY >= config.windowY && mouseY < config.windowY + config.windowHeight) {
#endif
                return overlay.get();
            }
        }
        return nullptr;
    }

    void OverlayManager::UpdateActiveScreen(bool anyHasSelection) {
        // 获取鼠标位置
        float mouseX, mouseY;
        SDL_GetGlobalMouseState(&mouseX, &mouseY);

        // 记录初始鼠标位置，只有鼠标真正移动后才触发蒙层销毁
        if (m_initialMousePos.x < 0) {
            m_initialMousePos = ImVec2(mouseX, mouseY);
        }
        if (!m_mouseHasMoved) {
            float dx = mouseX - m_initialMousePos.x;
            float dy = mouseY - m_initialMousePos.y;
            if (dx * dx + dy * dy > 4.0f) {  // 移动超过 2px 才算真正移动
                m_mouseHasMoved = true;
            }
        }

        // 查找鼠标所在的 Overlay
        if (ScreenOverlay* overlay = HitTestMouseOverlay(mouseX, mouseY)) {
            // 鼠标真正移动过才永久移除蒙层
            if (m_mouseHasMoved && overlay->IsDimmed()) {
                overlay->RemoveDim();
            }

            // 选区存在时不切换活跃屏幕
            if (!anyHasSelection && m_overlays.size() > 1 && overlay->GetDisplayId() != m_activeDisplayId) {
                ActivateOverlay(overlay, true);
            }
        }
    }

} // namespace shotlet
