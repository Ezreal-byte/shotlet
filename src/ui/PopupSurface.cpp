#include "PopupSurface.h"
#include "platform/Appearance.h"
#include "utils/UiFontUtils.h"
#include "utils/ImGuiUtils.h"
#include "utils/WindowUtils.h"
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlgpu3.h>

namespace shotlet {
namespace {
constexpr float kUiFontLogicalSize = 13.0f * 1.4f;
constexpr float kPaddingLogical = 12;
constexpr float kSpacingLogical = 6;
constexpr float kRoundingLogical = 6;
constexpr float kBorderLogicalWidth = 1;
constexpr Uint32 kColorTargetCount = 1;
}

PopupSurface::~PopupSurface() { Close(); }

bool PopupSurface::Create(const char* title, SDL_Point size, SDL_Window* parent) {
    if (m_window) return true;
    // SDL_GPU 不接受透明窗口标记，外轮廓由平台层裁剪，保持交换链的原有创建方式。
    constexpr SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN;
    m_window = parent
        ? SDL_CreatePopupWindow(parent, 0, 0, size.x, size.y, flags | SDL_WINDOW_TOOLTIP)
        : SDL_CreateWindow(title, size.x, size.y, flags | SDL_WINDOW_BORDERLESS | SDL_WINDOW_UTILITY | SDL_WINDOW_ALWAYS_ON_TOP);
    if (!m_window) return false;
    if (!SDL_ClaimWindowForGPUDevice(m_device, m_window)) { SDL_DestroyWindow(m_window); m_window = nullptr; return false; }
    auto* previous = ImGui::GetCurrentContext();
    m_context = ImGui::CreateContext();
    ImGui::SetCurrentContext(m_context);
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.FontDefault = ImGuiUtils::AddUiFont(*io.Fonts, kUiFontLogicalSize);
    m_sdlBackend = ImGui_ImplSDL3_InitForOther(m_window);
    ImGui_ImplSDLGPU3_InitInfo info{};
    info.Device = m_device;
    info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(m_device, m_window);
    info.MSAASamples = SDL_GPU_SAMPLECOUNT_1;
    info.SwapchainComposition = SDL_GPU_SWAPCHAINCOMPOSITION_SDR;
    info.PresentMode = SDL_GPU_PRESENTMODE_VSYNC;
    if (m_sdlBackend) m_gpuBackend = ImGui_ImplSDLGPU3_Init(&info);
    ImGui::SetCurrentContext(previous);
    if (!m_gpuBackend) { Close(); return false; }
    return true;
}

void PopupSurface::Close() {
    if (!m_window) return;
    SDL_WaitForGPUIdle(m_device);
    auto* previous = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(m_context);
    if (m_gpuBackend) ImGui_ImplSDLGPU3_Shutdown();
    if (m_sdlBackend) ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext(m_context);
    ImGui::SetCurrentContext(previous == m_context ? nullptr : previous);
    m_context = nullptr;
    m_sdlBackend = m_gpuBackend = false;
    SDL_ReleaseWindowFromGPUDevice(m_device, m_window);
    SDL_DestroyWindow(m_window);
    m_window = nullptr;
    m_scale = 0; m_theme.reset();
    m_roundedSize = {}; m_cornerRadius = 0;
    m_revealAfterRender = m_focusAfterRender = false;
}

void PopupSurface::Hide() {
    if (m_window) SDL_HideWindow(m_window);
    m_revealAfterRender = m_focusAfterRender = false;
}
bool PopupSurface::IsVisible() const { return m_window && !(SDL_GetWindowFlags(m_window) & SDL_WINDOW_HIDDEN); }
void PopupSurface::Show(bool focus) {
    if (!m_window) return;
    if (IsVisible()) { if (focus) SDL_RaiseWindow(m_window); return; }
    // 首帧完成前保持透明，避免状态栏下方闪出尚未绘制的黑色窗口。
    SDL_SetWindowOpacity(m_window, 0);
    SDL_ShowWindow(m_window);
    m_revealAfterRender = true; m_focusAfterRender = focus;
}
float PopupSurface::Scale() const { return m_window ? WindowUtils::GetDisplayScale(m_window).contentScale : 1; }

bool PopupSurface::ProcessEvent(const SDL_Event& event) {
    if (!m_window || SDL_GetWindowFromEvent(&event) != m_window) return false;
    const ImGuiUtils::ContextScope context(m_context);
    ImGui_ImplSDL3_ProcessEvent(&event);
    return true;
}

void PopupSurface::ApplyStyle(Theme theme) {
    const float scale = Scale();
    if (scale == m_scale && theme == m_theme) return;
    m_scale = scale; m_theme = theme;
    auto& style = ImGui::GetStyle();
    style = ImGuiStyle();
    if (theme == Theme::Dark) ImGui::StyleColorsDark(); else ImGui::StyleColorsLight();
    const auto& palette = GetHistoryPalette(theme);
    const auto set = [&](ImGuiCol color, ImU32 value) { style.Colors[color] = ImGui::ColorConvertU32ToFloat4(value); };
    set(ImGuiCol_WindowBg, palette.background); set(ImGuiCol_PopupBg, palette.panel);
    set(ImGuiCol_Text, palette.text); set(ImGuiCol_TextDisabled, palette.muted);
    set(ImGuiCol_Border, palette.border); set(ImGuiCol_HeaderHovered, palette.selected);
    set(ImGuiCol_Button, palette.panel); set(ImGuiCol_ButtonHovered, palette.selected);
    set(ImGuiCol_Header, palette.selected); set(ImGuiCol_HeaderActive, palette.selected);
    style.WindowPadding = ImVec2(kPaddingLogical, kPaddingLogical);
    style.ItemSpacing = ImVec2(kSpacingLogical, kSpacingLogical);
    style.FrameRounding = kRoundingLogical;
    style.WindowRounding = WindowCornerRadiusLogical;
    style.WindowBorderSize = kBorderLogicalWidth;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    ApplyWindowTheme(m_window, theme);
}

void PopupSurface::UpdateWindowShape() {
    SDL_Point size{};
    if (!SDL_GetWindowSize(m_window, &size.x, &size.y)) return;
    const float radius = WindowCornerRadiusLogical * Scale();
    if (size.x == m_roundedSize.x && size.y == m_roundedSize.y && radius == m_cornerRadius) return;
    ApplyPopupWindowCorners(m_window, radius);
    m_roundedSize = size; m_cornerRadius = radius;
}

void PopupSurface::Render(Theme theme, const std::function<void()>& content) {
    if (!IsVisible()) return;
    const ImGuiUtils::ContextScope context(m_context);
    ApplyStyle(theme);
    UpdateWindowShape();
    ImGui_ImplSDLGPU3_NewFrame(); ImGui_ImplSDL3_NewFrame(); ImGui::NewFrame();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("##popup-surface", nullptr, flags)) content();
    ImGui::End(); ImGui::Render();
    if (SubmitFrame() && m_revealAfterRender) {
        SDL_WaitForGPUIdle(m_device);
        SDL_SetWindowOpacity(m_window, 1);
        if (m_focusAfterRender) SDL_RaiseWindow(m_window);
        m_revealAfterRender = m_focusAfterRender = false;
    }
}

bool PopupSurface::SubmitFrame() {
    auto* command = SDL_AcquireGPUCommandBuffer(m_device);
    if (!command) return false;
    SDL_GPUTexture* texture = nullptr;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(command, m_window, &texture, nullptr, nullptr) || !texture) {
        SDL_CancelGPUCommandBuffer(command); return false;
    }
    auto* draw = ImGui::GetDrawData();
    ImGui_ImplSDLGPU3_PrepareDrawData(draw, command);
    SDL_GPUColorTargetInfo target{};
    target.texture = texture; target.load_op = SDL_GPU_LOADOP_CLEAR; target.store_op = SDL_GPU_STOREOP_STORE;
    auto* pass = SDL_BeginGPURenderPass(command, &target, kColorTargetCount, nullptr);
    if (pass) { ImGui_ImplSDLGPU3_RenderDrawData(draw, command, pass); SDL_EndGPURenderPass(pass); }
    return SDL_SubmitGPUCommandBuffer(command);
}
} // namespace shotlet
