#pragma once
#include "Theme.h"
#include <SDL3/SDL.h>
#include <functional>
#include <imgui.h>

namespace shotlet {
// 轻量弹层共用的 SDL/ImGui 渲染资源；只管理窗口与 GPU，不依赖历史业务。
class PopupSurface {
public:
    static constexpr float WindowCornerRadiusLogical = 12;
    explicit PopupSurface(SDL_GPUDevice* device) : m_device(device) {}
    ~PopupSurface();
    PopupSurface(const PopupSurface&) = delete;
    PopupSurface& operator=(const PopupSurface&) = delete;
    bool Create(const char* title, SDL_Point size, SDL_Window* parent = nullptr);
    void Close();
    void Hide();
    bool IsVisible() const;
    void Show(bool focus);
    bool ProcessEvent(const SDL_Event& event);
    void Render(Theme theme, const std::function<void()>& content);
    SDL_Window* Window() const { return m_window; }
    ImGuiContext* Context() const { return m_context; }
    float Scale() const;
private:
    void ApplyStyle(Theme theme);
    void UpdateWindowShape();
    bool SubmitFrame();
    SDL_GPUDevice* m_device;
    SDL_Window* m_window = nullptr;
    ImGuiContext* m_context = nullptr;
    bool m_sdlBackend = false, m_gpuBackend = false;
    bool m_revealAfterRender = false, m_focusAfterRender = false;
    float m_scale = 0;
    SDL_Point m_roundedSize{};
    float m_cornerRadius = 0;
    std::optional<Theme> m_theme;
};
} // namespace shotlet
