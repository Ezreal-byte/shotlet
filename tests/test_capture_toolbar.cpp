#include "ui/Toolbar.h"
#include "core/Canvas.h"
#include <cstdio>

namespace {
struct Actions { int ocr = 0, pin = 0, save = 0, record = 0, confirm = 0, cancel = 0; };

Actions ClickToolbar(bool editorMode) {
    ImGuiContext* previous = ImGui::GetCurrentContext();
    ImGuiContext* context = ImGui::CreateContext();
    Actions actions;
    {
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigInputTrickleEventQueue = false;
        io.DisplaySize = ImVec2(1200, 800);
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
        io.Fonts->SetTexID(1);
        shotlet::Canvas canvas;
        canvas.ReplaceScreenshotAndSelectAll(1, ImVec2(320, 240), ImVec2(320, 240), nullptr, 1280);
        shotlet::Toolbar toolbar;
        toolbar.SetCanvas(&canvas);
        toolbar.SetEditorMode(editorMode);
        toolbar.SetOcrCallback([&] { ++actions.ocr; });
        toolbar.SetPinCallback([&] { ++actions.pin; });
        toolbar.SetSaveCallback([&] { ++actions.save; });
        toolbar.SetRecordCallback([&] { ++actions.record; });
        toolbar.SetConfirmCallback([&] { ++actions.confirm; });
        toolbar.SetCancelCallback([&] { ++actions.cancel; });
        auto frame = [&] { ImGui::NewFrame(); toolbar.Render(); ImGui::Render(); };
        frame();
        frame();
        const auto bounds = toolbar.GetWindowRect();
        const float centerY = (bounds.Min.y + bounds.Max.y) / 2;
        const float step = shotlet::Toolbar::IconSize / 4;
        // 使用真实 ImGui 命中逐点点击整条工具栏，不依赖图标的排列下标或 GPU。
        for (float x = bounds.Min.x; x < bounds.Max.x; x += step) {
            io.AddMousePosEvent(x, centerY);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
            frame();
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
            frame();
        }
    }
    ImGui::DestroyContext(context);
    ImGui::SetCurrentContext(previous);
    return actions;
}
}

int main() {
    // SDK 与独立应用共用同一套工具栏：浮动与编辑器两种形态都必须提供全部输出动作，
    // 否则宿主用户在长截图编辑器里会丢失 OCR、贴图、保存或录制入口。
    const auto floating = ClickToolbar(false);
    if (!floating.ocr || !floating.pin || !floating.save || !floating.record || !floating.confirm) return 1;
    const auto editor = ClickToolbar(true);
    if (!editor.ocr || !editor.pin || !editor.save || !editor.confirm) return 2;
    std::puts("工具栏在浮动与编辑器形态下均提供 OCR、贴图、保存、录制和完成入口");
    return 0;
}
