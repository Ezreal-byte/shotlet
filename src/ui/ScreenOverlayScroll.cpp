#include "ScreenOverlay.h"

#if defined(SHOTLET_PLATFORM_MACOS) || defined(SHOTLET_PLATFORM_WINDOWS)

#include "core/Canvas.h"
#include "platform/FileService.h"

#include <cmath>

namespace shotlet {

void ScreenOverlay::PollScrollCapture() {
    if (m_shouldClose && m_scrollCapture) {
        m_scrollCapture->Cancel();
        return;
    }
    if (m_saveScrollResult && m_pendingScrollResult) {
        m_saveScrollResult = false;
        SaveFileWithDialog(m_pendingScrollResult->rgba.data(),
                           m_pendingScrollResult->width,
                           m_pendingScrollResult->height, m_config.language);
    }
    if (m_scrollRequested) {
        m_scrollRequested = false;
        if (m_pendingScrollResult) {
            if (m_onScrollImageReady && m_onScrollImageReady(m_pendingScrollResult)) {
                m_pendingScrollResult.reset();
                m_scrollMessage.clear();
            }
            return;
        }
        if (!m_scrollCapture && m_canvas && m_canvas->HasSelection()) {
            m_canvas->FlushPendingInput();
            m_canvas->SetTool(ToolType::None);
            const auto selection = m_canvas->GetSelection();
            const GlobalRect rect{
                m_config.windowX + int(std::ceil(selection.Min.x)),
                m_config.windowY + int(std::ceil(selection.Min.y)),
                int(std::floor(selection.Max.x)) - int(std::ceil(selection.Min.x)),
                int(std::floor(selection.Max.y)) - int(std::ceil(selection.Min.y))};
            m_scrollCapture = ScrollCaptureUI::Create(
                m_sdlWindow, m_config.displayId, rect, m_config.scaleFactor, m_scrollMessage, m_config.language, m_config.theme);
            m_selectionLocked = bool(m_scrollCapture);
            if (m_scrollCapture) HideNativeWindow();
        }
    }
    if (!m_scrollCapture) return;
    auto state = m_scrollCapture->Poll();
    if (state.state == ScrollCaptureState::Failed && !state.viewport) {
        m_scrollMessage = state.message;
        m_scrollCapture.reset();
        ShowNativeWindow();
#if defined(SHOTLET_PLATFORM_WINDOWS)
        ForceActivate();
#endif
        m_selectionLocked = false;
    } else if (state.state == ScrollCaptureState::Cancelled) {
        m_scrollCapture.reset();
        m_selectionLocked = false;
        m_shouldClose = true;
    } else if (state.state == ScrollCaptureState::Done && state.result) {
        const auto action = m_scrollCapture->TakeAction();
        if (action == ScrollCaptureAction::None) return;
        if (action == ScrollCaptureAction::Copy) {
            m_pendingScrollResult = state.result;
            m_scrollCapture.reset();
            m_scrollMessage.clear();
            // 交给主工具栏相同的完成流程复制，并按成功截图清理窗口。
            m_hasResult = true;
            m_shouldClose = true;
        } else if (action == ScrollCaptureAction::Save) {
            m_scrollCapture->SetToolbarVisible(false);
            const auto path = SaveFileWithDialog(state.result->rgba.data(),
                                                 state.result->width, state.result->height, m_config.language);
            if (path.empty()) {
                m_scrollCapture->SetToolbarVisible(true);
                return;
            }
            m_scrollCapture.reset();
            m_pendingScrollResult.reset();
            m_scrollMessage.clear();
            m_shouldClose = true;
        } else {
            m_scrollCapture.reset();
            if (m_onScrollImageReady && m_onScrollImageReady(state.result)) {
                m_pendingScrollResult.reset();
                m_scrollMessage.clear();
            } else {
                m_pendingScrollResult = state.result;
                m_scrollMessage = Tr(m_config.language, TextId::ScrollUploadFailed);
            }
        }
        m_selectionLocked = false;
    }
}

}  // namespace shotlet

#endif
