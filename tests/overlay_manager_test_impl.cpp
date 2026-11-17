// Test-only OverlayManager seams
// 真 OverlayManager.cpp 已链入测试目标（tests/CMakeLists.txt），
// 本文件只提供头文件声明的"测试专用方法"：向真实现注入可控的初始状态。
// 历史上这里曾是整套与生产分叉的重实现桩（路由逻辑与生产不一致且无测试保护），
// 已随编辑器路由单测一并移除。

#include "ui/OverlayManager.h"
#include "ui/ImageEditor.h"
#include "platform/MagnifierOverlay.h"
#include <memory>

namespace shotlet {

void OverlayManager::AddTestOverlay(std::unique_ptr<ScreenOverlay> overlay) {
    m_overlays.push_back(std::move(overlay));
}

void OverlayManager::SetActiveDisplayId(int id) {
    m_activeDisplayId = id;
}

void OverlayManager::SetMagnifierVisible(bool visible) {
    if (!m_magnifier && visible) {
        m_magnifier = std::make_unique<MagnifierOverlay>();
        m_magnifier->Initialize();
    }
    if (m_magnifier) {
        if (visible) {
            m_magnifier->Show();
        } else {
            m_magnifier->Hide();
        }
    }
}

bool OverlayManager::IsMagnifierVisible() const {
    return m_magnifier && m_magnifier->IsVisible();
}

void OverlayManager::SetImageEditorForTest(std::unique_ptr<ImageEditor> editor) {
    m_imageEditor = std::move(editor);
}

} // namespace shotlet
