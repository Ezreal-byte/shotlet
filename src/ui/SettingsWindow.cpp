#define IMGUI_DEFINE_MATH_OPERATORS
#include "SettingsWindow.h"
#include "platform/Appearance.h"
#include "platform/FontProvider.h"
#include "platform/TransparentWindow.h"
#include "ui/WindowChrome.h"
#include "utils/FormatUtils.h"
#include "utils/ImGuiUtils.h"
#include "utils/UiGpuUtils.h"
#include "utils/UiFontUtils.h"
#include "utils/WindowUtils.h"
#include "utils/ApplicationIcon.h"
#include "utils/Logger.h"
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>
#include <imgui_internal.h>
#include <algorithm>
#include <array>
#if defined(SHOTLET_PLATFORM_MACOS)
#include "platform/ImageEditorWindow.h"
#endif

namespace shotlet {
    namespace {
        // 窗口和控件均使用逻辑尺寸基准；设置行留白按用户要求保留实际像素单位。
        namespace WindowLayout {
            constexpr int LogicalWidth = 640;
            constexpr int LogicalHeight = 560;
            constexpr float SidebarLogicalWidth = 160;
            // Windows 无边框窗口与其他窗口共用 42 逻辑像素标题栏；macOS 沿用原生关闭按钮的 48 布局。
#if defined(SHOTLET_PLATFORM_WINDOWS)
            constexpr float TitlebarHeight = WindowChrome::TitleBarHeight;
#else
            constexpr float TitlebarHeight = 48;
#endif
            constexpr float DragStartX = 85;
            constexpr float HiddenOpacity = 0;
            constexpr float VisibleOpacity = 1;
            constexpr int MinimumLogicalExtent = 1;
            constexpr int UnrestrictedExtent = 0;
        }

        namespace Typography {
            constexpr float BodySize = 18.2f;
            constexpr float PageTitleSize = 24;
            constexpr float SidebarTitleSize = 20;
            constexpr float BrandSize = 26;
        }

        namespace StyleLayout {
            constexpr float CornerRadius = 7;
            constexpr float ScrollbarWidth = 7;
            constexpr float ControlLabelTop = 5;
            const ImVec2 FramePadding{12, 7};
            const ImVec2 ItemSpacing{10, 8};
        }

        namespace SidebarLayout {
            const ImVec2 BrandPosition{26, 60};
            constexpr float MenuTop = 120;
            constexpr float ItemHeight = 40;
            constexpr float ItemGap = 4;
            constexpr float ItemStride = ItemHeight + ItemGap;
            constexpr float IconX = 32;
            const ImVec2 LabelOffset{58, 10};
            constexpr float FooterBottom = 36;
        }

        namespace ContentLayout {
            constexpr float HorizontalMargin = 36;
            constexpr float VerticalMargin = 55;
            constexpr float HeadingSeparatorGap = 12;
            constexpr float ItemVerticalPaddingPixels = 10;
            constexpr float SectionGap = 10;
            constexpr float SavedNoticeBottom = 33;
        }

        namespace ChoiceLayout {
            constexpr float Width = 184;
            constexpr float HintTop = 47;
        }

        namespace RecorderLayout {
            // 高度由 ImGui 按当前字号和 FramePadding 自动计算，与通用页下拉框一致。
            const ImVec2 Size{235, 0};
            constexpr float OutlineWidth = 1.5f;
        }

        namespace StorageLayout {
            constexpr float LabelTop = 5;
            constexpr float HintTop = 42;
            const ImVec2 ChangeButtonSize{105, 0};
        }

        namespace SwitchLayout {
            const ImVec2 Size{32, 19.2f};
            constexpr float CornerRadius = 9.6f;
            constexpr float ThumbRadius = 7.2f;
            constexpr float OffCenterX = 9.6f;
            constexpr float OnCenterX = 22.4f;
            constexpr float CenterY = 9.6f;
        } // namespace SwitchLayout

        namespace DialogLayout {
            constexpr float Width = 420;
            const ImVec2 Padding{22, 22};
            const ImVec2 ActionSize{160, 36};
            constexpr float ActionsGap = 12;
        }

        namespace IconLayout {
            constexpr float StrokeWidth = 1.5f;
            constexpr int SettingsVertexCount = 6;
            constexpr float SettingsRadius = 8;
            constexpr float SettingsHoleRadius = 3.4f;
            constexpr int CircleSegments = 20;
            constexpr float FullTurn = IM_PI * 2;
            constexpr float TopAngle = -IM_PI / 2;
            constexpr float CornerRadius = 2;
            constexpr float KeyboardLogicalWidth = 16;
            constexpr float KeyboardSvgWidth = 20;
            constexpr float KeyboardSvgHeight = 16;
            constexpr float KeyboardSvgCornerRadius = 2;
            const ImVec2 KeyboardSvgHalfSize{KeyboardSvgWidth / 2, KeyboardSvgHeight / 2};
            // SVG 坐标以 (12, 12) 为中心转换，保留两排按键错位排列和独立空格键。
            constexpr auto KeyboardSvgKeyOffsets = std::to_array<ImVec2>({
                {-6, -4}, {-2, -4}, {2, -4}, {6, -4},
                {-4, 0}, {0, 0}, {4, 0}
            });
            const ImVec2 KeyboardSvgSpacebarStart{-5, 4};
            const ImVec2 KeyboardSvgSpacebarEnd{5, 4};
            const ImVec2 StorageHalfSize{8, 6.5f};
            constexpr float StorageDividerY = 1.5f;
            const ImVec2 StorageIndicatorOffset{4.4f, 4};
            constexpr float StorageIndicatorRadius = 1.1f;
            const ImVec2 LogHalfSize{6, 8};
            constexpr float LogLineHalfWidth = 3;
            constexpr auto LogLineOffsetsY = std::to_array<float>({-4, 0, 4});
        }

        constexpr auto SavedNoticeDuration = std::chrono::seconds(3);
        constexpr const char *ClearHistoryPopup = "clear-history";

        enum PageIndex { GeneralPage, HotkeysPage, StoragePage, LogsPage };

        enum RecorderIndex { CaptureRecorder, HistoryRecorder, CopyLatestRecorder };

        constexpr auto PageTitles = std::to_array<TextId>({
            TextId::SettingsGeneral, TextId::SettingsHotkeys, TextId::SettingsStorage, TextId::SettingsLogs
        });

        ImFont *LoadSettingsFont(ImFontAtlas &atlas) {
            ImFont* font = ImGuiUtils::AddUiFont(atlas, Typography::BodySize);
#if defined(SHOTLET_PLATFORM_MACOS)
            // 中文字体缺少部分修饰键符号，合并系统字体，避免 Command 等按键显示为问号。
            ImFontConfig symbols;
            symbols.MergeMode = true;
            atlas.AddFontFromFileTTF("/System/Library/Fonts/Apple Symbols.ttf", Typography::BodySize, &symbols);
#endif
            return font;
        }

        bool InitializeGpuBackend(SDL_GPUDevice *device, SDL_Window *window) {
            ImGui_ImplSDLGPU3_InitInfo info{};
            info.Device = device;
            info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(device, window);
            info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
            info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
            return ImGui_ImplSDLGPU3_Init(&info);
        }

        void DrawSettingsIcon(ImDrawList &draw, ImVec2 center, ImU32 color, float scale) {
            // 六个顶点等角分布，首个顶点正上方，与之相对的顶点正下方。
            std::array<ImVec2, IconLayout::SettingsVertexCount> vertices;
            for (size_t i = 0; i < vertices.size(); ++i) {
                const float angle = IconLayout::TopAngle + i * IconLayout::FullTurn / vertices.size();
                vertices[i] = center + ImVec2(std::cos(angle), std::sin(angle)) * IconLayout::SettingsRadius * scale;
            }
            const float stroke = IconLayout::StrokeWidth * scale;
            draw.AddPolyline(vertices.data(), int(vertices.size()), color, ImDrawFlags_Closed, stroke);
            draw.AddCircle(center, IconLayout::SettingsHoleRadius * scale, color, IconLayout::CircleSegments, stroke);
        }

        void DrawKeyboardIcon(ImDrawList &draw, ImVec2 center, ImU32 color, float scale) {
            const float stroke = IconLayout::StrokeWidth * scale;
            // 按 SVG 比例调整轮廓，同时保持原菜单图标的逻辑宽度、线宽和颜色。
            const float svgScale = IconLayout::KeyboardLogicalWidth / IconLayout::KeyboardSvgWidth * scale;
            const auto halfSize = IconLayout::KeyboardSvgHalfSize * svgScale;
            draw.AddRect(center - halfSize, center + halfSize, color,
                         IconLayout::KeyboardSvgCornerRadius * svgScale, ImDrawFlags_None, stroke);
            const float capRadius = stroke / 2;
            // SVG 中长度仅 0.01 的按键短线，其外观由圆端帽决定，可直接绘制为圆点。
            for (const auto offset: IconLayout::KeyboardSvgKeyOffsets) {
                draw.AddCircleFilled(center + offset * svgScale, capRadius, color, IconLayout::CircleSegments);
            }
            const ImVec2 spacebar[] = {
                center + IconLayout::KeyboardSvgSpacebarStart * svgScale,
                center + IconLayout::KeyboardSvgSpacebarEnd * svgScale
            };
            draw.AddPolyline(spacebar, int(std::size(spacebar)), color, ImDrawFlags_None, stroke);
            for (const auto endpoint: spacebar) {
                draw.AddCircleFilled(endpoint, capRadius, color, IconLayout::CircleSegments);
            }
        }

        void DrawStorageIcon(ImDrawList &draw, ImVec2 center, ImU32 color, float scale) {
            const float stroke = IconLayout::StrokeWidth * scale;
            const auto halfSize = IconLayout::StorageHalfSize * scale;
            draw.AddRect(center - halfSize, center + halfSize, color,
                         IconLayout::CornerRadius * scale, ImDrawFlags_None, stroke);
            const ImVec2 left{-IconLayout::StorageHalfSize.x, IconLayout::StorageDividerY};
            const ImVec2 right{IconLayout::StorageHalfSize.x, IconLayout::StorageDividerY};
            draw.AddLine(center + left * scale, center + right * scale, color, stroke);
            draw.AddCircleFilled(center + IconLayout::StorageIndicatorOffset * scale,
                                 IconLayout::StorageIndicatorRadius * scale, color);
        }

        void DrawLogIcon(ImDrawList &draw, ImVec2 center, ImU32 color, float scale) {
            const auto halfSize = IconLayout::LogHalfSize * scale;
            const float stroke = IconLayout::StrokeWidth * scale;
            draw.AddRect(center - halfSize, center + halfSize, color,
                         IconLayout::CornerRadius * scale, ImDrawFlags_None, stroke);
            for (const auto offsetY : IconLayout::LogLineOffsetsY) {
                const ImVec2 start{-IconLayout::LogLineHalfWidth, offsetY};
                const ImVec2 end{IconLayout::LogLineHalfWidth, offsetY};
                draw.AddLine(center + start * scale, center + end * scale, color, stroke);
            }
        }

        void DrawSidebarIcon(ImDrawList &draw, ImVec2 center, int page, ImU32 color, float scale) {
            switch (page) {
                case GeneralPage: DrawSettingsIcon(draw, center, color, scale);
                    break;
                case HotkeysPage: DrawKeyboardIcon(draw, center, color, scale);
                    break;
                case StoragePage: DrawStorageIcon(draw, center, color, scale);
                    break;
                case LogsPage: DrawLogIcon(draw, center, color, scale);
                    break;
            }
        }

        void DrawHistorySwitch(ImVec2 position, bool enabled, float scale) {
            auto &draw = *ImGui::GetWindowDrawList();
            draw.AddRectFilled(position, position + SwitchLayout::Size * scale,
                               enabled ? Colors::SettingsAccent : Colors::DisabledSwitch, SwitchLayout::CornerRadius * scale);
            const ImVec2 center{enabled ? SwitchLayout::OnCenterX : SwitchLayout::OffCenterX, SwitchLayout::CenterY};
            draw.AddCircleFilled(position + center * scale, SwitchLayout::ThumbRadius * scale, Colors::White);
        }
    } // namespace

    SettingsWindow::SettingsWindow(SDL_GPUDevice *device, Preferences preferences, Language language,
                                   Theme theme, Callbacks callbacks)
        : m_device(device), m_preferences(std::move(preferences)), m_language(language),
          m_theme(theme), m_callbacks(std::move(callbacks)) {
    }

    SettingsWindow::~SettingsWindow() { Close(); }

    bool SettingsWindow::CreateWindow() {
        SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
#if defined(SHOTLET_PLATFORM_WINDOWS)
        // Windows 使用无边框窗口：固定尺寸，仅保留最小化/关闭控制组与拖动。
        flags |= SDL_WINDOW_BORDERLESS;
#endif
        m_window = SDL_CreateWindow(Tr(m_language, TextId::SettingsTitle),
                                    WindowLayout::LogicalWidth, WindowLayout::LogicalHeight,
                                    flags);
        if (!m_window) return false;
#if defined(SHOTLET_PLATFORM_WINDOWS)
        if (!ApplicationIcon::SetWindowIcon(m_window))
            Logger::Warning("[SettingsWindow] Cannot set application icon: {}", SDL_GetError());
#endif
#if defined(SHOTLET_PLATFORM_MACOS)
        // 固定尺寸的设置窗口只保留关闭按钮。
        SetupHistoryBrowserWindow(m_window, m_theme, /*closeOnly=*/true);
#endif
        UpdateDisplayScale();
        if (!SDL_ClaimWindowForGPUDevice(m_device, m_window)) {
            SDL_DestroyWindow(m_window);
            m_window = nullptr;
            m_pixelDensity = 0;
            return false;
        }
        SDL_SetWindowPosition(m_window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_SetWindowHitTest(m_window, [](SDL_Window *window, const SDL_Point *point, void *data) {
            const auto &settings = *static_cast<SettingsWindow *>(data);
            if (settings.m_context) {
                ImGuiUtils::ContextScope context(settings.m_context);
                // 保留控件按压和弹出菜单的输入；仅有键盘焦点时仍允许拖动空白标题栏。
                const bool pressingControl = ImGui::IsAnyItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
                if (pressingControl ||
                    ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
                    return SDL_HITTEST_NORMAL;
            }
#if defined(SHOTLET_PLATFORM_WINDOWS)
            int width = 0; SDL_GetWindowSize(window, &width, nullptr);
            // 原生命中早于本次鼠标移动的 ImGui 帧，按钮区域始终按事件坐标排除。
            if (WindowChrome::IsWindowControlsPoint(ImVec2(float(point->x), float(point->y)), float(width),
                    WindowLayout::TitlebarHeight * settings.m_scale, settings.m_scale, /*includeMaximize=*/false))
                return SDL_HITTEST_NORMAL;
#endif
            const bool titlebar = point->y < WindowLayout::TitlebarHeight * settings.m_scale;
            const bool outsideButtons = point->x > WindowLayout::DragStartX * settings.m_scale;
            return titlebar && outsideButtons ? SDL_HITTEST_DRAGGABLE : SDL_HITTEST_NORMAL;
        }, this);
        if (InitializeImGui()) return true;
        Close();
        return false;
    }

    bool SettingsWindow::InitializeImGui() {
        // CreateContext 自身也可能切换上下文，因此必须在创建之前保存调用方状态。
        ImGuiUtils::ContextScope context(ImGui::GetCurrentContext());
        m_context = ImGui::CreateContext();
        ImGui::SetCurrentContext(m_context);
        auto &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.FontDefault = LoadSettingsFont(*io.Fonts);
        // 任一后端初始化失败，由 CreateWindow 统一释放已创建的资源。
        if (!ImGui_ImplSDL3_InitForOther(m_window) || !InitializeGpuBackend(m_device, m_window)) return false;
        ApplyStyle();
        ApplyWindowTheme(m_window, m_theme);
        return true;
    }

    bool SettingsWindow::Show() {
        if (!m_window && !CreateWindow()) return false;
        m_close = false;
        // 先在透明窗口上完成首帧，避免展示未绘制的 GPU 画面。
        SDL_SetWindowOpacity(m_window, WindowLayout::HiddenOpacity);
        SDL_ShowWindow(m_window);
        RenderFrame();
        SDL_WaitForGPUIdle(m_device);
        SDL_SetWindowOpacity(m_window, WindowLayout::VisibleOpacity);
        SDL_RaiseWindow(m_window);
        ForceWindowFocus(m_window);
        return true;
    }

    bool SettingsWindow::IsVisible() const {
        return m_window && !(SDL_GetWindowFlags(m_window) & SDL_WINDOW_HIDDEN);
    }

    void SettingsWindow::Hide() {
        m_recording = NoRecorder;
        if (!m_window) return;
#if defined(SHOTLET_PLATFORM_MACOS)
        HideHistoryBrowserWindow(m_window);
#else
        SDL_HideWindow(m_window);
        SDL_SyncWindow(m_window);
#endif
    }

    void SettingsWindow::DestroyImGui() {
        if (!m_context) return;
        auto *previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(m_context);
        if (ImGui::GetIO().BackendRendererUserData) ImGui_ImplSDLGPU3_Shutdown();
        if (ImGui::GetIO().BackendPlatformUserData) ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(m_context);
        // 不能把刚销毁的上下文恢复为当前上下文。
        ImGui::SetCurrentContext(previous == m_context ? nullptr : previous);
        m_context = nullptr;
    }

    void SettingsWindow::Close() {
        if (!m_window) return;
        SDL_WaitForGPUIdle(m_device);
        DestroyImGui();
        SDL_ReleaseWindowFromGPUDevice(m_device, m_window);
        SDL_DestroyWindow(m_window);
        m_window = nullptr;
        m_pixelDensity = 0;
        m_recording = NoRecorder;
    }

    void SettingsWindow::UpdateDisplayScale() {
        const auto scale = WindowUtils::GetDisplayScale(m_window);
        const bool contentScaleChanged = scale.contentScale != m_scale;
        // Windows 改变系统缩放时像素密度仍为 1，不能只监听像素密度变化。
        if (scale.pixelDensity != m_pixelDensity || contentScaleChanged) {
            ResizeForContentScale(scale.contentScale);
            m_pixelDensity = scale.pixelDensity;
        }
        if (!contentScaleChanged) return;
        m_scale = scale.contentScale;
        if (m_context) ApplyStyle();
    }

    void SettingsWindow::ResizeForContentScale(float scale) {
        const int width = std::max(WindowLayout::MinimumLogicalExtent,
                                   int(std::lround(WindowLayout::LogicalWidth * scale)));
        const int height = std::max(WindowLayout::MinimumLogicalExtent,
                                    int(std::lround(WindowLayout::LogicalHeight * scale)));
        int x = 0, y = 0, oldWidth = 0, oldHeight = 0;
        SDL_GetWindowPosition(m_window, &x, &y);
        SDL_GetWindowSize(m_window, &oldWidth, &oldHeight);
        // 跨屏时先解除旧尺寸限制，允许逻辑尺寸向两个方向变化，再重新固定。
        SDL_SetWindowMinimumSize(m_window, WindowLayout::UnrestrictedExtent, WindowLayout::UnrestrictedExtent);
        SDL_SetWindowMaximumSize(m_window, WindowLayout::UnrestrictedExtent, WindowLayout::UnrestrictedExtent);
        SDL_SetWindowSize(m_window, width, height);
        SDL_SetWindowMinimumSize(m_window, width, height);
        SDL_SetWindowMaximumSize(m_window, width, height);
        // 保持窗口中心，避免调整大小后又落入另一块显示器，反复触发缩放。
        SDL_SetWindowPosition(m_window, x + (oldWidth - width) / 2, y + (oldHeight - height) / 2);
        SDL_SyncWindow(m_window);
    }

    void SettingsWindow::ApplyStyle() {
        auto &style = ImGui::GetStyle();
        style = ImGuiStyle();
        ApplyImGuiTheme(style, m_theme);
        // 侧栏线条直接使用几何抗锯齿，避免采样依赖随字形加载而更新的字体图集。
        style.AntiAliasedLinesUseTex = false;
        const auto &colors = GetSettingsPalette(m_theme);
        style.WindowPadding = ImVec2();
        style.ChildBorderSize = style.WindowBorderSize = 0;
        style.FrameRounding = style.PopupRounding = StyleLayout::CornerRadius;
        style.FramePadding = StyleLayout::FramePadding;
        style.ItemSpacing = StyleLayout::ItemSpacing;
        style.ScrollbarSize = StyleLayout::ScrollbarWidth;
        auto set = [&](ImGuiCol index, ImU32 color) { style.Colors[index] = ImGui::ColorConvertU32ToFloat4(color); };
        set(ImGuiCol_WindowBg, colors.background);
        set(ImGuiCol_ChildBg, colors.background);
        set(ImGuiCol_Button, colors.control);
        set(ImGuiCol_FrameBg, colors.control);
        set(ImGuiCol_Text, colors.text);
        set(ImGuiCol_Separator, colors.separator);
        set(ImGuiCol_Header, Colors::SettingsAccent);
        set(ImGuiCol_CheckMark, Colors::SettingsAccent);
        style.ScaleAllSizes(m_scale);
        style.FontScaleDpi = m_scale;
    }

    void SettingsWindow::SetTheme(Theme theme) {
        if (theme == m_theme) return;
        m_theme = theme;
        if (!m_context) return;
        ImGuiUtils::ContextScope context(m_context);
        ApplyStyle();
        ApplyWindowTheme(m_window, theme);
    }

    void SettingsWindow::SetPreferences(const Preferences &preferences, Language language) {
        m_preferences = preferences;
        m_language = language;
        if (m_window) SDL_SetWindowTitle(m_window, Tr(language, TextId::SettingsTitle));
    }

    bool SettingsWindow::Save(Preferences preferences) {
        m_error.clear();
        // 注册和持久化由应用层完成；成功后才更新界面，失败保留当前录入及错误。
        if (!m_callbacks.save(preferences, m_error)) return false;
        m_preferences = std::move(preferences);
        m_recording = NoRecorder;
        m_savedUntil = std::chrono::steady_clock::now() + SavedNoticeDuration;
        return true;
    }

    bool SettingsWindow::ProcessEvent(const SDL_Event &event) {
        if (!m_window || SDL_GetWindowFromEvent(&event) != m_window) return false;
        ImGuiUtils::ContextScope context(m_context);
        ImGui_ImplSDL3_ProcessEvent(&event);
        switch (event.type) {
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED: m_close = true;
                break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: m_recording = NoRecorder;
                break;
            case SDL_EVENT_KEY_DOWN:
                if (!event.key.repeat) HandleKeyDown(event);
                break;
        }
        return true;
    }

    void SettingsWindow::HandleKeyDown(const SDL_Event &event) {
        if (event.key.key == SDLK_ESCAPE) {
            if (m_recording == NoRecorder) m_close = true;
            else {
                m_recording = NoRecorder;
                m_error.clear();
            }
            return;
        }
        const bool modifierOnly = event.key.scancode >= SDL_SCANCODE_LCTRL && event.key.scancode <= SDL_SCANCODE_RGUI;
        if (m_recording == NoRecorder || modifierOnly) return;
        const bool clear = event.key.key == SDLK_BACKSPACE || event.key.key == SDLK_DELETE;
        const auto binding = clear ? HotkeyBinding::Disabled() : HotkeyFromEvent(event);
        if (!binding.IsValid()) {
            m_error = Tr(m_language, TextId::HotkeySettingsInvalid);
            return;
        }
        auto preferences = m_preferences;
        switch (m_recording) {
            case CaptureRecorder: preferences.captureHotkey = binding; break;
            case HistoryRecorder: preferences.historyHotkey = binding; break;
            case CopyLatestRecorder: preferences.copyLatestHotkey = binding; break;
            default: return;
        }
        Save(preferences);
    }

    bool SettingsWindow::RenderChoiceRow(TextId title, TextId hint, const char *id, int &selected,
                                         std::span<const char * const> choices) {
        ImGuiUtils::PaddedRow row(ContentLayout::ItemVerticalPaddingPixels / m_pixelDensity);
        const float width = ImGui::GetContentRegionAvail().x;
        const float top = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(top + StyleLayout::ControlLabelTop * m_scale);
        ImGui::TextUnformatted(Tr(m_language, title));
        ImGui::SameLine(width - ChoiceLayout::Width * m_scale);
        ImGui::SetCursorPosY(top);
        ImGui::SetNextItemWidth(ChoiceLayout::Width * m_scale);
        const bool changed = ImGui::Combo(id, &selected, choices.data(), int(choices.size()));
        ImGui::SetCursorPosY(top + ChoiceLayout::HintTop * m_scale);
        ImGuiUtils::MutedWrappedText(Tr(m_language, hint));
        return changed;
    }

    void SettingsWindow::RenderAppearance() {
        const auto choices = std::to_array<const char *>({
            Tr(m_language, TextId::ThemeAuto), Tr(m_language, TextId::ThemeLight), Tr(m_language, TextId::ThemeDark)
        });
        int selected = int(m_preferences.theme);
        if (!RenderChoiceRow(TextId::ThemeMenu, TextId::SettingsAppearanceHint, "##appearance", selected,
                             choices)) return;
        auto preferences = m_preferences;
        preferences.theme = ThemePreference(selected);
        Save(preferences);
    }

    void SettingsWindow::RenderLanguage() {
        const auto choices = std::to_array<const char *>({
            Tr(m_language, TextId::SettingsLanguageAuto), "简体中文", "English"
        });
        int selected = int(m_preferences.language);
        if (!RenderChoiceRow(TextId::SettingsLanguage, TextId::SettingsLanguageHint, "##language", selected,
                             choices)) return;
        auto preferences = m_preferences;
        preferences.language = LanguagePreference(selected);
        Save(preferences);
    }

    void SettingsWindow::RenderGeneral() {
        RenderAppearance();
        RenderLanguage();
    }

    void SettingsWindow::RenderRecorderButton(int index, HotkeyBinding binding) {
        const bool recording = m_recording == index;
        const auto position = ImGui::GetCursorScreenPos();
        if (recording) ImGui::PushStyleColor(ImGuiCol_Button, GetSettingsPalette(m_theme).recorderActive);
        const auto text = recording
                              ? std::string(Tr(m_language, TextId::SettingsRecording))
                              : HotkeyLabel(binding, m_language);
        if (ImGui::Button(text.c_str(), RecorderLayout::Size * m_scale)) {
            m_recording = index;
            m_error.clear();
        }
        if (recording) {
            ImGui::PopStyleColor();
            ImGui::GetWindowDrawList()->AddRect(position, ImGui::GetItemRectMax(), Colors::SettingsAccent,
                                                StyleLayout::CornerRadius * m_scale, ImDrawFlags_None,
                                                RecorderLayout::OutlineWidth * m_scale);
        }
        // 点击其他区域结束录入，避免后续普通按键意外覆盖快捷键。
        if (m_recording == index && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsItemHovered())
            m_recording = NoRecorder;
    }

    void SettingsWindow::RenderHotkeyRow(int index, TextId label, HotkeyBinding binding) {
        ImGuiUtils::PaddedRow row(ContentLayout::ItemVerticalPaddingPixels / m_pixelDensity);
        const float top = ImGui::GetCursorPosY();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosY(top + StyleLayout::ControlLabelTop * m_scale);
        ImGui::TextUnformatted(Tr(m_language, label));
        ImGui::SameLine(width - RecorderLayout::Size.x * m_scale);
        ImGui::SetCursorPosY(top);
        ImGui::PushID(index);
        RenderRecorderButton(index, binding);
        ImGui::PopID();
    }

    void SettingsWindow::RenderHotkeys() {
        RenderHotkeyRow(CaptureRecorder, TextId::SettingsCaptureHotkey, m_preferences.captureHotkey);
        RenderHotkeyRow(HistoryRecorder, TextId::SettingsHistoryHotkey, m_preferences.historyHotkey);
        RenderHotkeyRow(CopyLatestRecorder, TextId::CopyLatestCapture, m_preferences.copyLatestHotkey);
        ImGuiUtils::VerticalSpace(ContentLayout::SectionGap * m_scale);
        ImGuiUtils::MutedWrappedText(Tr(m_language, TextId::SettingsHotkeyHint));
    }

    void SettingsWindow::RenderHistoryPreference() {
        ImGuiUtils::PaddedRow row(ContentLayout::ItemVerticalPaddingPixels / m_pixelDensity);
        const float top = ImGui::GetCursorPosY();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosY(top + StorageLayout::LabelTop * m_scale);
        ImGui::TextUnformatted(Tr(m_language, TextId::SettingsHistoryEnabled));
        ImGui::SameLine(width - SwitchLayout::Size.x * m_scale);
        ImGui::SetCursorPosY(top);
        const auto position = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##history-switch", SwitchLayout::Size * m_scale)) {
            m_error.clear();
            if (m_callbacks.enableHistory(!m_storage.enabled, m_error)) m_storage.enabled = !m_storage.enabled;
        }
        DrawHistorySwitch(position, m_storage.enabled, m_scale);
        ImGui::SetCursorPosY(top + StorageLayout::HintTop * m_scale);
        ImGuiUtils::MutedWrappedText(Tr(m_language, TextId::SettingsHistoryHint));
    }

    void SettingsWindow::RenderStorageLocation() {
        ImGuiUtils::PaddedRow row(ContentLayout::ItemVerticalPaddingPixels / m_pixelDensity);
        const float top = ImGui::GetCursorPosY();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosY(top + StyleLayout::ControlLabelTop * m_scale);
        ImGui::TextUnformatted(Tr(m_language, TextId::SettingsStorageLocation));
        ImGui::SameLine(width - StorageLayout::ChangeButtonSize.x * m_scale);
        ImGui::SetCursorPosY(top);
        if (ImGui::Button(Tr(m_language, TextId::SettingsChooseDirectory), StorageLayout::ChangeButtonSize * m_scale)) {
            const auto directory = m_callbacks.chooseDirectory();
            if (!directory.empty()) {
                auto preferences = m_preferences;
                preferences.storageDirectory = directory;
                Save(preferences);
            }
        }
        const auto directory = m_storage.directory.u8string();
        ImGuiUtils::MutedWrappedText(reinterpret_cast<const char *>(directory.c_str()));
        ImGuiUtils::MutedWrappedText(Tr(m_language, TextId::SettingsStorageLocationHint));
    }

    void SettingsWindow::RenderStorageUsage() {
        ImGuiUtils::PaddedRow row(ContentLayout::ItemVerticalPaddingPixels / m_pixelDensity);
        ImGui::TextUnformatted(Tr(m_language, TextId::SettingsStorageUsage));
        const auto usage = FormatText(m_language, TextId::SettingsStorageUsageValue,
                                      FormatUtils::ByteSize(m_storage.used), m_storage.count);
        ImGui::TextDisabled("%s", usage.c_str());
        const auto space = FormatText(m_language, TextId::SettingsDiskSpace, FormatUtils::ByteSize(m_storage.available),
                                      FormatUtils::ByteSize(m_storage.capacity));
        ImGui::TextDisabled("%s", space.c_str());
    }

    void SettingsWindow::RenderLogLocation() {
        ImGuiUtils::PaddedRow row(ContentLayout::ItemVerticalPaddingPixels / m_pixelDensity);
        const float top = ImGui::GetCursorPosY();
        const float width = ImGui::GetContentRegionAvail().x;
        ImGui::SetCursorPosY(top + StyleLayout::ControlLabelTop * m_scale);
        ImGui::TextUnformatted(Tr(m_language, TextId::SettingsLogLocation));
        ImGui::SameLine(width - StorageLayout::ChangeButtonSize.x * m_scale);
        ImGui::SetCursorPosY(top);
        if (ImGui::Button(Tr(m_language, TextId::SettingsChooseDirectory), StorageLayout::ChangeButtonSize * m_scale))
            ChangeLogDirectory();
        const auto directory = m_logPath.parent_path().u8string();
        ImGuiUtils::MutedWrappedText(reinterpret_cast<const char*>(directory.c_str()));
        ImGuiUtils::MutedWrappedText(Tr(m_language, TextId::SettingsLogLocationHint));
    }

    void SettingsWindow::ChangeLogDirectory() {
        const auto directory = m_callbacks.chooseLogDirectory();
        if (directory.empty()) return;
        m_error.clear();
        if (!m_callbacks.saveLogDirectory(directory, m_error)) return;
        m_preferences.logDirectory = directory;
        // 界面先同步目录，下一帧再由应用层刷新实际文件路径。
        m_logPath = directory / m_logPath.filename();
        m_savedUntil = std::chrono::steady_clock::now() + SavedNoticeDuration;
    }

    void SettingsWindow::RenderLogs() {
        RenderLogLocation();
        ImGuiUtils::PaddedRow row(ContentLayout::ItemVerticalPaddingPixels / m_pixelDensity, /*separator=*/false);
        if (ImGui::Button(Tr(m_language, TextId::SettingsViewLog))) {
            m_error.clear();
            m_callbacks.viewLog(m_error);
        }
        ImGuiUtils::MutedWrappedText(Tr(m_language, TextId::SettingsViewLogHint));
    }

    void SettingsWindow::RenderClearHistoryButton() {
        ImGuiUtils::PaddedRow row(ContentLayout::ItemVerticalPaddingPixels / m_pixelDensity, /*separator=*/false);
        if (ImGuiUtils::DestructiveButton(Tr(m_language, TextId::SettingsClearHistory), GetThemePalette(m_theme)))
            ImGui::OpenPopup(ClearHistoryPopup);
        ImGuiUtils::MutedWrappedText(Tr(m_language, TextId::SettingsClearHint));
    }

    void SettingsWindow::ConfirmClearHistory() {
        m_error.clear();
        if (!m_callbacks.clearHistory(m_error)) return;
        m_storage.used = m_storage.count = 0;
        m_savedUntil = std::chrono::steady_clock::now() + SavedNoticeDuration;
        ImGui::CloseCurrentPopup();
    }

    void SettingsWindow::RenderClearConfirmation() {
        ImGui::SetNextWindowSize(ImVec2(DialogLayout::Width * m_scale, 0), ImGuiCond_Appearing);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, DialogLayout::Padding * m_scale);
        if (ImGui::BeginPopupModal(ClearHistoryPopup, nullptr,
                                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
            const float textWidth = DialogLayout::Width - DialogLayout::Padding.x - DialogLayout::Padding.x;
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textWidth * m_scale);
            ImGui::TextUnformatted(Tr(m_language, TextId::SettingsClearConfirm));
            ImGui::PopTextWrapPos();
            ImGuiUtils::VerticalSpace(DialogLayout::ActionsGap * m_scale);
            if (ImGui::Button(Tr(m_language, TextId::HistoryCancel), DialogLayout::ActionSize * m_scale))
                ImGui::CloseCurrentPopup();
            ImGui::SameLine();
            if (ImGuiUtils::DestructiveButton(Tr(m_language, TextId::HistoryConfirm), GetThemePalette(m_theme),
                                              DialogLayout::ActionSize * m_scale))
                ConfirmClearHistory();
            RenderError();
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    void SettingsWindow::RenderStorage() {
        ImGui::BeginDisabled(!m_storage.ready);
        RenderHistoryPreference();
        RenderStorageLocation();
        RenderStorageUsage();
        RenderClearHistoryButton();
        ImGui::EndDisabled();
        // 确认弹窗在禁用区域外绘制，避免存储状态变化时无法取消弹窗。
        RenderClearConfirmation();
    }

    void SettingsWindow::RenderSidebarItem(int page, float width) {
        const float top = SidebarLayout::MenuTop + page * SidebarLayout::ItemStride;
        ImGui::SetCursorPos(ImVec2(0, top * m_scale));
        ImGui::PushID(page);
        const bool selected = m_page == page;
        const auto position = ImGui::GetCursorScreenPos();
        const ImVec2 size{width, SidebarLayout::ItemHeight * m_scale};
        if (ImGui::InvisibleButton("page", size)) {
            m_page = page;
            m_recording = NoRecorder;
            m_error.clear();
        }
        auto &draw = *ImGui::GetWindowDrawList();
        if (selected || ImGui::IsItemHovered())
            draw.AddRectFilled(position, position + size,
                               selected ? Colors::SettingsAccent : GetSettingsPalette(m_theme).sidebarHovered);
        const auto color = selected ? Colors::White : ImGui::GetColorU32(ImGuiCol_Text);
        const ImVec2 iconOffset{SidebarLayout::IconX, SidebarLayout::ItemHeight / 2};
        DrawSidebarIcon(draw, position + iconOffset * m_scale, page, color, m_scale);
        draw.AddText(ImGui::GetFont(), Typography::SidebarTitleSize * m_scale,
                     position + SidebarLayout::LabelOffset * m_scale, color, Tr(m_language, PageTitles[page]));
        ImGui::PopID();
    }

    void SettingsWindow::RenderSidebar(float width) {
        auto &draw = *ImGui::GetWindowDrawList();
        const float height = ImGui::GetIO().DisplaySize.y;
        draw.AddRectFilled(ImVec2(), ImVec2(width, height), GetSettingsPalette(m_theme).sidebar);
        draw.AddText(ImGui::GetFont(), Typography::BrandSize * m_scale, SidebarLayout::BrandPosition * m_scale,
                     ImGui::GetColorU32(ImGuiCol_Text), "Shotlet");
        for (size_t page = 0; page < PageTitles.size(); ++page) RenderSidebarItem(int(page), width);
        const ImVec2 footer{SidebarLayout::BrandPosition.x * m_scale, height - SidebarLayout::FooterBottom * m_scale};
        draw.AddText(footer, ImGui::GetColorU32(ImGuiCol_TextDisabled), "Shotlet " SHOTLET_VERSION);
    }

    void SettingsWindow::RenderError() {
        if (m_error.empty()) return;
        ImGui::PushStyleColor(ImGuiCol_Text, GetThemePalette(m_theme).danger);
        ImGui::TextWrapped("%s", m_error.c_str());
        ImGui::PopStyleColor();
    }

    void SettingsWindow::RenderPageContent() {
        switch (m_page) {
            case GeneralPage: RenderGeneral();
                break;
            case HotkeysPage: RenderHotkeys();
                break;
            case StoragePage: RenderStorage();
                break;
            case LogsPage: RenderLogs();
                break;
        }
    }

    void SettingsWindow::RenderPage(float sidebarWidth) {
        const auto &display = ImGui::GetIO().DisplaySize;
        const float horizontalMargin = ContentLayout::HorizontalMargin * m_scale;
        const float verticalMargin = ContentLayout::VerticalMargin * m_scale;
        ImGui::SetCursorPos(ImVec2(sidebarWidth + horizontalMargin, verticalMargin));
        const ImVec2 size{
            display.x - sidebarWidth - horizontalMargin - horizontalMargin,
            display.y - verticalMargin - verticalMargin
        };
        ImGui::BeginChild("settings-content", size);
        ImGui::PushFont(nullptr, Typography::PageTitleSize);
        ImGui::TextUnformatted(Tr(m_language, PageTitles[m_page]));
        ImGui::PopFont();
        ImGuiUtils::VerticalSpace(ContentLayout::HeadingSeparatorGap * m_scale);
        ImGuiUtils::SeparatorWithoutSpacing();
        RenderPageContent();
        if (!m_error.empty()) {
            ImGuiUtils::VerticalSpace(ContentLayout::SectionGap * m_scale);
            RenderError();
        }
        ImGui::EndChild();
    }

    void SettingsWindow::RenderSavedNotice(float sidebarWidth) {
        if (std::chrono::steady_clock::now() >= m_savedUntil) return;
        const ImVec2 position{
            sidebarWidth + ContentLayout::HorizontalMargin * m_scale,
            ImGui::GetIO().DisplaySize.y - ContentLayout::SavedNoticeBottom * m_scale
        };
        ImGui::GetWindowDrawList()->AddText(
            position, ImGui::GetColorU32(ImGuiCol_TextDisabled), Tr(m_language, TextId::SettingsSaved));
    }

    void SettingsWindow::HandleWindowAction(WindowChrome::WindowAction action) {
        // 固定尺寸无边框窗口只提供最小化与关闭。
        if (!m_window) return;
        switch (action) {
            case WindowChrome::WindowAction::Minimize:
                SDL_MinimizeWindow(m_window);
                break;
            case WindowChrome::WindowAction::ToggleMaximize:
                break;
            case WindowChrome::WindowAction::Close:
                m_close = true;
                break;
        }
    }

    void SettingsWindow::RenderContent() {
        ImGui::SetNextWindowPos(ImVec2());
        ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
        // 点击侧边栏等交互会让 ImGui 把获得焦点的全屏主窗口提到渲染最上层，
        // 盖住右上角的共享窗口控制组；与编辑器视口一致禁止聚焦置顶。
        ImGui::Begin("##settings", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoBringToFrontOnFocus);
#if defined(SHOTLET_PLATFORM_WINDOWS)
        // 无边框窗口顶部绘制标题文字，与历史窗口标题位置一致。
        ImGui::GetWindowDrawList()->AddText(
            ImVec2(20 * m_scale, (WindowLayout::TitlebarHeight * m_scale - ImGui::GetTextLineHeight()) * .5f),
            ImGui::GetColorU32(ImGuiCol_Text), Tr(m_language, TextId::SettingsTitle));
#endif
        const float sidebarWidth = WindowLayout::SidebarLogicalWidth * m_scale;
        RenderSidebar(sidebarWidth);
        RenderPage(sidebarWidth);
        RenderSavedNotice(sidebarWidth);
        ImGui::End();
#if defined(SHOTLET_PLATFORM_WINDOWS)
        // 固定尺寸窗口仅提供最小化与关闭控制组，底色与内容背景一致保持无缝。
        WindowChrome::RenderWindowControls("SettingsWindowControls", WindowLayout::TitlebarHeight * m_scale,
                                           m_theme, m_language, m_scale, /*maximized=*/false,
                                           /*includeMaximize=*/false, GetSettingsPalette(m_theme).background,
                                           nullptr, [this](WindowChrome::WindowAction a) { HandleWindowAction(a); });
#endif
    }

    void SettingsWindow::SubmitFrame() {
        ImGuiUtils::SubmitGpuDrawData(m_device, m_window, ImGui::GetDrawData());
    }

    void SettingsWindow::RenderFrame() {
        if (m_close) {
            Close();
            return;
        }
        if (!WindowUtils::IsDrawable(m_window)) return;
        ImGuiUtils::ContextScope context(m_context);
        UpdateDisplayScale();
#if defined(SHOTLET_PLATFORM_MACOS)
        LayoutHistoryBrowserWindow(m_window, WindowLayout::TitlebarHeight * m_scale);
#endif
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        const auto displaySize = ImGui::GetIO().DisplaySize;
        if (displaySize.x <= 0 || displaySize.y <= 0) return;
        ImGui::NewFrame();
        RenderContent();
        ImGui::Render();
        SubmitFrame();
    }
} // namespace shotlet
