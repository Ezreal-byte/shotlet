#pragma once

#include "scrollcapture/ScrollCaptureController.h"
#include "ui/Theme.h"

#include <memory>
#include <string>

struct SDL_Window;

namespace shotlet {

enum class ScrollCaptureAction { None, Edit, Save, Copy };

// 平台滚动截图界面统一入口；实现负责平台采帧、输入接管和辅助窗口。
class ScrollCaptureUI {
public:
    virtual ~ScrollCaptureUI() = default;
    virtual ScrollCaptureSnapshot Poll() = 0;
    virtual void Cancel() = 0;
    virtual void SetTheme(Theme theme) = 0;
    virtual ScrollCaptureAction TakeAction() = 0;
    virtual void SetToolbarVisible(bool visible) = 0;

    static std::unique_ptr<ScrollCaptureUI> Create(
        SDL_Window* window,
        int displayId,
        GlobalRect selection,
        double scale,
        std::string& error,
        Language language = Language::English,
        Theme theme = Theme::Dark);
};

}  // namespace shotlet
