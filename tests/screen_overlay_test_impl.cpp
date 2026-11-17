// Test-only minimal implementation of ScreenOverlay
// Does not depend on Canvas.h (which has ImGui dependencies)
// Only implements the methods needed for unit testing state logic

#include "ui/ScreenOverlay.h"
#include "core/Canvas.h"
#include "ui/SubToolbar.h"
#include "ui/Toolbar.h"

namespace shotlet {
void ScreenOverlay::SetTheme(Theme theme) { m_config.theme = theme; }

ScreenOverlay::ScreenOverlay() = default;

ScreenOverlay::~ScreenOverlay() {
    Destroy();
}

void ScreenOverlay::SetActive(bool active) {
    m_active = active;
}

void ScreenOverlay::Destroy() {
    if (!m_created) return;
    m_canvas = nullptr;
    m_created = false;
    m_hasResult = false;
    m_shouldClose = false;
    m_active = false;
}

void ScreenOverlay::ProcessEvent(const SDL_Event&) {}

// 真实实现隐藏窗口并暂停渲染；桩只记录状态（OverlayManager 不读取，但符号必须存在）
void ScreenOverlay::Hide() { m_hidden = true; }

namespace {
// 供测试断言（声明在 test_overlay_manager.cpp）：编辑器独占时 overlay 渲染是否被旁路
int g_renderFrameCalls = 0;
}

int ScreenOverlayRenderFrameCallCountForTest() {
    return g_renderFrameCalls;
}

void ScreenOverlay::RenderFrame() {
    ++g_renderFrameCalls;
}

bool ScreenOverlay::IsMouseInWindow() const { return false; }

bool ScreenOverlay::HasWindowID(uint32_t) const { return false; }

bool ScreenOverlay::CreateNativeWindow() { return false; }
void ScreenOverlay::DestroyNativeWindow() { m_nativeWindow = nullptr; }
bool ScreenOverlay::CreateSDLWindow() { return false; }
void ScreenOverlay::DestroySDLWindow() { m_sdlWindow = nullptr; m_gpuDevice = nullptr; }
bool ScreenOverlay::InitializeImGui() { return false; }
void ScreenOverlay::ShutdownImGui() { m_imguiContext = nullptr; }
void ScreenOverlay::UpdateNativeBackground(const unsigned char*, int, int, int) {}
void ScreenOverlay::ShowNativeWindow() {}
void ScreenOverlay::HideNativeWindow() {}
void ScreenOverlay::ClearNativeBackground() {}
void ScreenOverlay::HideSDLWindow() {}
void ScreenOverlay::RevealSDLWindow() {}
bool ScreenOverlay::PreCreate(const Config&) { return false; }
bool ScreenOverlay::SetCaptureData(std::unique_ptr<unsigned char[]>, int, int, int) { return false; }
void ScreenOverlay::ShowOverlay() {}
void ScreenOverlay::ShowBackground() {}
void ScreenOverlay::ResetForReuse() {}
void ScreenOverlay::ForceActivate() {}
void ScreenOverlay::RemoveDim() { m_dimmed = false; }

} // namespace shotlet
