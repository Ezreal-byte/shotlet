// Test-only minimal implementation of ImageEditor
// 与 screen_overlay_test_impl.cpp 同模式：不依赖 SDL 窗口/GPU/ImGui 后端，
// 只提供真 OverlayManager.cpp 链入测试后所需的最小符号与状态观测。

#include "ui/ImageEditor.h"
#include "core/Canvas.h"
#include "core/CursorManager.h"
#include "ui/Toolbar.h"
#include "ui/SubToolbar.h"

#include <unordered_map>

namespace shotlet {
void ImageEditor::SetTheme(Theme theme) { m_config.theme = theme; }
namespace {

// 哑指针仅作路由断言标识：OverlayManager 对 SDL_Window*/ImGuiContext* 只做
// 转发与判空，绝不解引用。绝不传入任何 SDL/ImGui API。
SDL_Window* const kTestWindowSentinel = reinterpret_cast<SDL_Window*>(0x1);
ImGuiContext* const kTestImGuiSentinel = reinterpret_cast<ImGuiContext*>(0x2);

// 每个桩实例一份测试观测状态（RenderFrame 调用计数），随析构清理
struct EditorTestState {
    int renderFrameCalls = 0;
    ImVec2 selectionScreenOrigin{-1, -1};  // GetSelectionScreenOrigin 桩返回值（测试可控）
};
std::unordered_map<const ImageEditor*, EditorTestState>& EditorStates() {
    static std::unordered_map<const ImageEditor*, EditorTestState> states;
    return states;
}

} // namespace

// 供测试断言（声明在 test_overlay_manager.cpp）：编辑器独占分支是否真的渲染了编辑器
int ImageEditorRenderFrameCallCountForTest(const ImageEditor* editor) {
    const auto& states = EditorStates();
    const auto it = states.find(editor);
    return it == states.end() ? 0 : it->second.renderFrameCalls;
}

// 供路由测试控制 GetSelectionScreenOrigin 桩返回值
void SetImageEditorSelectionScreenOriginForTest(const ImageEditor* editor, ImVec2 origin) {
    EditorStates()[editor].selectionScreenOrigin = origin;
}

ImageEditor::ImageEditor(const ScreenOverlay::Config& config,
                          std::shared_ptr<const ScrollImage> image,
                          SDL_GPUDevice* gpuDevice)
    : m_config(config), m_gpuDevice(gpuDevice), m_image(std::move(image)) {
    // GetCanvas()/GetSDLWindow()/GetImGuiContext() 路由断言需要非空返回值
    m_canvas = std::make_unique<Canvas>();
    m_sdlWindow = kTestWindowSentinel;
    m_imguiContext = kTestImGuiSentinel;
    EditorStates()[this] = {};
}

ImageEditor::~ImageEditor() {
    Destroy();
    EditorStates().erase(this);
}

bool ImageEditor::Create() { return false; }

void ImageEditor::Destroy() {
    m_canvas.reset();
    m_toolbar.reset();
    m_subToolbar.reset();
    m_cursorManager.reset();
    m_created = false;
}

void ImageEditor::ProcessEvent(const SDL_Event&) {}

void ImageEditor::RenderFrame() {
    ++EditorStates()[this].renderFrameCalls;
}

bool ImageEditor::HasWindowID(uint32_t) const { return false; }

void ImageEditor::RenderViewport() {}

bool ImageEditor::CreateSDLWindow() { return false; }

void ImageEditor::DestroySDLWindow() { m_sdlWindow = nullptr; }

bool ImageEditor::InitializeImGui() { return false; }

void ImageEditor::ShutdownImGui() {}

void ImageEditor::SetRoutingStateForTest(bool hasResult, bool shouldClose,
                                         bool pinRequested, bool saveRequested,
                                         bool ocrRequested) {
    m_hasResult = hasResult;
    m_shouldClose = shouldClose;
    m_pinRequested = pinRequested;
    m_saveRequested = saveRequested;
    m_ocrRequested = ocrRequested;
}

ImVec2 ImageEditor::GetSelectionScreenOrigin() const {
    const auto it = EditorStates().find(this);
    return it == EditorStates().end() ? ImVec2(-1, -1) : it->second.selectionScreenOrigin;
}

} // namespace shotlet
