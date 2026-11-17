#include "platform/LocalizedText.h"
#include "scrollcapture/ScrollCaptureUI.h"

#if !defined(SHOTLET_PLATFORM_WINDOWS)
#error "WinScrollCaptureUI.cpp 只能在 Windows 上编译"
#endif

#include "platform/PlatformWin32.h"
#include "scrollcapture/ScrollCaptureTypes.h"
#include "scrollcapture/WinFrameSource.h"
#include "scrollcapture/WinScrollDriver.h"
#include "ui/Toolbar.h"
#include "utils/Colors.h"
#include "utils/Logger.h"

#include <SDL3/SDL.h>
#include <dwmapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <mutex>

namespace shotlet {
namespace {

constexpr int kEditButtonId = 1001;
constexpr int kSaveButtonId = 1002;
constexpr int kCancelButtonId = 1003;
constexpr int kFinishButtonId = 1004;
constexpr wchar_t kControlClassName[] = L"ShotletScrollCaptureControl";
constexpr wchar_t kHintClassName[] = L"ShotletScrollCaptureHint";
constexpr wchar_t kMaskClassName[] = L"ShotletScrollCaptureMask";
constexpr wchar_t kBorderClassName[] = L"ShotletScrollCaptureBorder";
constexpr wchar_t kPreviewClassName[] = L"ShotletScrollCapturePreview";

constexpr COLORREF SelectionBorderColor() {
    constexpr ImU32 color = Colors::SelectionBorder;
    return RGB((color >> IM_COL32_R_SHIFT) & 255,
               (color >> IM_COL32_G_SHIFT) & 255,
               (color >> IM_COL32_B_SHIFT) & 255);
}

class Backend final : public ScrollCaptureBackend {
public:
    Backend(GlobalRect rect, HWND control, HWND preview, HWND hint,
            bool controlIntersectsSelection, bool previewIntersectsSelection,
            bool hintIntersectsSelection, Language language)
        : m_rect(rect), m_control(control), m_preview(preview), m_hint(hint),
          m_hideControl(controlIntersectsSelection), m_hidePreview(previewIntersectsSelection),
          m_hideHint(hintIntersectsSelection), m_language(language) {}

    bool Start(ScrollImage& first, std::function<void(int64_t)> callback,
               std::string& error) override {
        m_callback = std::move(callback);
        if (!m_driver.Start(m_rect, [this](int64_t delta) {
                const int direction = delta > 0 ? 1 : -1;
                int expected = 0;
                m_inputDirection.compare_exchange_strong(expected, direction);
                if (m_inputDirection.load() != direction) {
                    m_reversed.store(true);
                    return;
                }
                m_reversed.store(false);
                m_pending.fetch_add(delta);
            })) {
            Logger::Warning("[ScrollCapture] input failed: {}", m_driver.GetLastErrorMessage());
            error = Tr(m_language, TextId::ScrollInputFailed);
            return false;
        }
        if (!m_source.Start(m_rect)) {
            Logger::Warning("[ScrollCapture] source failed: {}", m_source.GetLastErrorMessage());
            error = Tr(m_language, TextId::ScrollCaptureFailed);
            return false;
        }
        SetControlVisible(false);
        const bool captured = m_source.Capture(first);
        SetControlVisible(true);
        if (!captured) {
            Logger::Warning("[ScrollCapture] frame failed: {}", m_source.GetLastErrorMessage());
            error = Tr(m_language, TextId::ScrollCaptureFailed);
            return false;
        }
        m_pending.store(0);
        m_driver.SetPassThroughPaused(false);
        m_lastFrame = first;
        Logger::Info("[WinScrollCapture][backend] started firstFrame={}x{} selection=({},{} {}x{})",
                     first.width, first.height, m_rect.x, m_rect.y, m_rect.w, m_rect.h);
        return true;
    }

    bool CaptureStep(int64_t inject, ScrollImage& image, bool& stable,
                     std::string& error) override {
        Logger::Debug("[WinScrollCapture][backend] capture step begin inject={}", inject);
        if (!ValidateTarget(error)) return false;
        SetControlVisible(false);
        ScrollImage baseline;
        const bool passThrough = m_driver.PassThroughInput();
        if (passThrough) m_driver.SetPassThroughPaused(true);
        if (passThrough) baseline = m_lastFrame;
        else if (!m_source.Capture(baseline)) {
            SetControlVisible(true);
            Logger::Warning("[ScrollCapture] frame failed: {}", m_source.GetLastErrorMessage());
            error = Tr(m_language, TextId::ScrollCaptureFailed);
            return false;
        }
        if (inject != 0 && !passThrough) m_driver.Inject(inject);
        Logger::Debug("[WinScrollCapture][backend] input mode={} baseline={}x{} gate=gesture-first",
                     passThrough ? "pass-through" : "controlled", baseline.width, baseline.height);
        const bool captured = m_source.WaitForStableFrame(baseline, image, stable);
        SetControlVisible(true);
        if (!captured) {
            Logger::Warning("[ScrollCapture] frame failed: {}", m_source.GetLastErrorMessage());
            error = Tr(m_language, m_cancelled.load() ? TextId::ScrollCancelled : TextId::ScrollCaptureFailed);
            return false;
        }
        m_lastFrame = image;
        Logger::Debug("[WinScrollCapture][backend] capture step end inject={} image={}x{} stable={}",
                     inject, image.width, image.height, stable);
        return true;
    }

    bool ValidateTarget(std::string& error) override {
        const auto telemetry = m_driver.DrainWheelTelemetry();
        if (telemetry.events > 0) {
            Logger::Debug("[WinScrollCapture][hook] events={} inside={} ignoredInjected={} lastPoint=({}, {}) lastDelta={} flags=0x{:x} extraInfo=0x{:x} selection=({},{} {}x{})",
                         telemetry.events, telemetry.inside, telemetry.ignoredInjected,
                         telemetry.x, telemetry.y, telemetry.delta, telemetry.flags,
                         telemetry.extraInfo, m_rect.x, m_rect.y, m_rect.w, m_rect.h);
        }
        ForwardPendingInput();
        if (m_driver.ValidateTarget(error)) return true;
        Logger::Warning("[ScrollCapture] target failed: {}", error);
        error = Tr(m_language, m_driver.GetTargetErrorId());
        return false;
    }

    void DiscardPendingInput() override {
        // 本步仍在进行中，转发输入仅用于刷新方向提示。
        ForwardPendingInput();
        m_passThroughAccumulator = 0;
        m_driver.SetPassThroughPaused(false);
    }
    bool CancelRequested() const override { return m_driver.CancelRequested(); }
    void RequestCancel() override {
        m_cancelled.store(true);
        m_driver.RequestStop();
        m_source.RequestStop();
    }
    void Stop() override {
        m_driver.Stop();
        m_source.Stop();
    }

private:
    void ForwardPendingInput() {
        const int64_t reverseNotice = ConsumeReverseNotice(m_reversed, m_inputDirection.load());
        const int64_t delta = m_pending.exchange(0);
        int64_t intentDelta = delta;
        if (m_driver.PassThroughInput()) {
            m_passThroughAccumulator += delta;
            intentDelta = 0;
            const int64_t threshold = 120;
            if (std::abs(m_passThroughAccumulator) >= threshold) {
                intentDelta = m_passThroughAccumulator;
                m_passThroughAccumulator = 0;
            }
        }
        if (intentDelta && m_callback) {
            Logger::Debug("[WinScrollCapture][input] aggregated rawDelta={} intent={}",
                         intentDelta, intentDelta > 0 ? 1 : -1);
            m_callback(intentDelta > 0 ? 1 : -1);
        }
        if (reverseNotice != 0 && m_callback) m_callback(reverseNotice);
    }
    void SetControlVisible(bool visible) {
        const bool toggleControl = m_hideControl && m_control;
        const bool togglePreview = m_hidePreview && m_preview;
        const bool toggleHint = m_hideHint && m_hint;
        if (!toggleControl && !togglePreview && !toggleHint) return;
        if (toggleControl) {
            ShowWindow(m_control, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
        if (togglePreview) ShowWindow(m_preview, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
        if (toggleHint) {
            if (!visible) m_hintWasVisible = IsWindowVisible(m_hint);
            ShowWindow(m_hint, visible && m_hintWasVisible ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
        DwmFlush();
    }

    GlobalRect m_rect;
    HWND m_control = nullptr;
    HWND m_preview = nullptr;
    HWND m_hint = nullptr;
    bool m_hideControl = false;
    bool m_hidePreview = false;
    bool m_hideHint = false;
    bool m_hintWasVisible = false;
    WinScrollDriver m_driver;
    const Language m_language;
    Theme m_theme = Theme::Dark;
    WinFrameSource m_source;
    std::atomic<int64_t> m_pending{0};
    std::atomic<int> m_inputDirection{0};
    std::atomic<bool> m_reversed{false};
    std::atomic<bool> m_cancelled{false};
    std::function<void(int64_t)> m_callback;
    ScrollImage m_lastFrame;
    int64_t m_passThroughAccumulator = 0;
};

class UI final : public ScrollCaptureUI {
public:
    UI(HWND overlay, GlobalRect selection, RECT monitor, Language language, Theme theme)
        : m_language(language), m_theme(theme), m_controller(language), m_overlay(overlay), m_selection(selection), m_monitor(monitor) {}

    ~UI() override {
        m_controller.Cancel();
        m_controller.Join();
        DestroyAuxiliaryWindows();
        if (m_overlayWasVisible && m_overlay && IsWindow(m_overlay)) {
            ShowWindow(m_overlay, SW_SHOWNOACTIVATE);
        }
    }

    bool Start(std::string& error) {
        RegisterWindowClasses();
        m_overlayWasVisible = IsWindowVisible(m_overlay) != FALSE;
        ShowWindow(m_overlay, SW_HIDE);
        DwmFlush();
        if (!CreateAuxiliaryWindows(error)) {
            if (m_overlayWasVisible) ShowWindow(m_overlay, SW_SHOWNOACTIVATE);
            return false;
        }
        auto backend = std::make_unique<Backend>(
            m_selection, m_control, m_preview, m_hint,
            m_controlIntersectsSelection, m_previewIntersectsSelection, m_hintIntersectsSelection, m_language);
        if (!m_controller.Start(std::move(backend))) {
            error = Tr(m_language, TextId::ScrollControllerFailed);
            return false;
        }
        return true;
    }

    void SetTheme(Theme theme) override {
        m_theme = theme;
        if (!m_control) return;
        std::string error;
        if (!UpdateControlBitmap(error)) Logger::Warning("[ScrollCapture] theme refresh failed: {}", error);
    }
    void Cancel() override { m_controller.Cancel(); }
    void SetToolbarVisible(bool visible) override {
        if (m_control) ShowWindow(m_control, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    }
    ScrollCaptureAction TakeAction() override {
        const auto action = m_action;
        m_action = ScrollCaptureAction::None;
        return action;
    }

    ScrollCaptureSnapshot Poll() override {
        auto state = m_controller.Snapshot();
        if (state.state == ScrollCaptureState::Cancelled && !m_controller.IsStopped()) {
            state.state = ScrollCaptureState::Finalizing;
            state.message = Tr(m_language, TextId::ScrollCancelling);
        }
        if (state.epoch == m_epoch && state.revision == m_revision) return state;
        m_epoch = state.epoch;
        m_revision = state.revision;
        if (m_reverseScrollBlocked != state.reverseScrollBlocked) {
            m_reverseScrollBlocked = state.reverseScrollBlocked;
            UpdateHint();
        }
        Logger::Debug("[WinScrollCapture][ui] snapshot epoch={} revision={} state={} totalHeight={} viewport={}x{} preview={}x{} result={}x{} message={}",
                     state.epoch, state.revision, int(state.state), state.totalHeight,
                     state.viewport ? state.viewport->width : 0,
                     state.viewport ? state.viewport->height : 0,
                     state.preview ? state.preview->width : 0,
                     state.preview ? state.preview->height : 0,
                     state.result ? state.result->width : 0,
                     state.result ? state.result->height : 0, state.message);
        const auto shownPreview = state.preview ? state.preview : state.viewport;
        if (shownPreview && shownPreview != m_lastPreview) {
            UpdatePreview(*shownPreview);
            m_lastPreview = shownPreview;
        }
        return state;
    }

    void OnCommand(int id) {
        if (id == kEditButtonId || id == kSaveButtonId) {
            m_action = id == kEditButtonId ? ScrollCaptureAction::Edit : ScrollCaptureAction::Save;
            m_controller.Finish();
        } else if (id == kFinishButtonId) {
            m_action = ScrollCaptureAction::Copy;
            m_controller.Finish();
        } else if (id == kCancelButtonId) m_controller.Cancel();
    }

private:
    static LRESULT CALLBACK ControlProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        UI* self = reinterpret_cast<UI*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<UI*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_COMMAND) {
            self->OnCommand(LOWORD(wParam));
            return 0;
        }
        if (self && message == WM_DRAWITEM) {
            self->DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
            return TRUE;
        }
        if (self && message == WM_PAINT) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            RECT bounds{};
            GetClientRect(window, &bounds);
            self->PaintBitmap(dc, self->m_controlBitmap, 0, 0,
                              bounds.right, bounds.bottom, 0, 0);
            EndPaint(window, &paint);
            return 0;
        }
        if (self && message == WM_CLOSE) {
            self->m_controller.Cancel();
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK MaskProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK BorderProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            HBRUSH border = CreateSolidBrush(SelectionBorderColor());
            FillRect(dc, &paint.rcPaint, border);
            DeleteObject(border);
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    static LRESULT CALLBACK HintProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        auto* self = reinterpret_cast<UI*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<UI*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (message == WM_NCHITTEST) return HTTRANSPARENT;
        if (self && message == WM_PAINT) {
            PAINTSTRUCT paint{};
            HDC dc = BeginPaint(window, &paint);
            FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            HGDIOBJ oldFont = SelectObject(dc, self->m_hintFont);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, Colors::ToNativeRgb(self->m_reverseScrollBlocked ? Colors::ScrollWarning : Colors::White));
            RECT bounds{};
            GetClientRect(window, &bounds);
            DrawTextW(dc, TrWide(self->m_language, self->m_reverseScrollBlocked ? TextId::ScrollReverseHint : TextId::ScrollHint).c_str(),
                      -1, &bounds, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
            SelectObject(dc, oldFont);
            EndPaint(window, &paint);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    static void RegisterWindowClasses() {
        static std::once_flag once;
        std::call_once(once, [] {
            WNDCLASSEXW control{sizeof(control)};
            control.lpfnWndProc = ControlProc;
            control.hInstance = GetModuleHandleW(nullptr);
            control.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
            control.hbrBackground = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
            control.lpszClassName = kControlClassName;
            RegisterClassExW(&control);

            WNDCLASSEXW hint{sizeof(hint)};
            hint.lpfnWndProc = HintProc;
            hint.hInstance = GetModuleHandleW(nullptr);
            hint.lpszClassName = kHintClassName;
            RegisterClassExW(&hint);
            WNDCLASSEXW mask{sizeof(mask)};
            mask.lpfnWndProc = MaskProc;
            mask.hInstance = GetModuleHandleW(nullptr);
            mask.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            mask.lpszClassName = kMaskClassName;
            RegisterClassExW(&mask);
            WNDCLASSEXW border{sizeof(border)};
            border.lpfnWndProc = BorderProc;
            border.hInstance = GetModuleHandleW(nullptr);
            border.hbrBackground = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
            border.lpszClassName = kBorderClassName;
            RegisterClassExW(&border);
            WNDCLASSEXW preview{sizeof(preview)};
            preview.lpfnWndProc = PreviewProc;
            preview.hInstance = GetModuleHandleW(nullptr);
            preview.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
            preview.lpszClassName = kPreviewClassName;
            RegisterClassExW(&preview);
        });
    }

    static LRESULT CALLBACK PreviewProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
        UI* self = reinterpret_cast<UI*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            self = static_cast<UI*>(create->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == WM_PAINT) {
            self->PaintPreview(window);
            return 0;
        }
        return DefWindowProcW(window, message, wParam, lParam);
    }

    static void PaintBitmap(HDC dc, HBITMAP bitmap, int x, int y,
                            int width, int height, int sourceX, int sourceY) {
        if (!bitmap) return;
        HDC memoryDc = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(memoryDc, bitmap);
        BitBlt(dc, x, y, width, height, memoryDc, sourceX, sourceY, SRCCOPY);
        SelectObject(memoryDc, old);
        DeleteDC(memoryDc);
    }

    void DrawButton(const DRAWITEMSTRUCT& item) {
        HDC dc = item.hDC;
        const RECT& r = item.rcItem;
        const int sourceX = ControlPixels(8 + (int(item.CtlID) - kEditButtonId) * Toolbar::ScrollCaptureButtonStep);
        PaintBitmap(dc, m_controlBitmap, r.left, r.top, r.right - r.left,
                    r.bottom - r.top, sourceX, ControlPixels(Toolbar::ScrollCaptureButtonInsetY));
        if (item.itemState & ODS_FOCUS) DrawFocusRect(dc, &r);
    }

    int ControlPixels(int value) const {
        return int(std::lround(value * m_controlScale));
    }

    bool CreateAuxiliaryWindows(std::string& error) {
        const std::array<RECT, 4> rectangles{{
            {m_monitor.left, m_monitor.top, m_monitor.right, m_selection.y},
            {m_monitor.left, m_selection.y + m_selection.h, m_monitor.right, m_monitor.bottom},
            {m_monitor.left, m_selection.y, m_selection.x, m_selection.y + m_selection.h},
            {m_selection.x + m_selection.w, m_selection.y, m_monitor.right, m_selection.y + m_selection.h}}};
        for (size_t i = 0; i < rectangles.size(); ++i) {
            const RECT& rect = rectangles[i];
            if (rect.right <= rect.left || rect.bottom <= rect.top) continue;
            m_masks[i] = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                    WS_EX_LAYERED | WS_EX_TRANSPARENT,
                kMaskClassName, L"", WS_POPUP,
                rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (m_masks[i]) {
                SetLayeredWindowAttributes(m_masks[i], 0, 112, LWA_ALPHA);
                ShowWindow(m_masks[i], SW_SHOWNOACTIVATE);
            }
        }
        const std::array<RECT, 4> borders{{
            {m_selection.x - 1, m_selection.y - 1,
             m_selection.x + m_selection.w + 1, m_selection.y},
            {m_selection.x - 1, m_selection.y + m_selection.h,
             m_selection.x + m_selection.w + 1, m_selection.y + m_selection.h + 1},
            {m_selection.x - 1, m_selection.y,
             m_selection.x, m_selection.y + m_selection.h},
            {m_selection.x + m_selection.w, m_selection.y,
             m_selection.x + m_selection.w + 1, m_selection.y + m_selection.h}}};
        for (size_t i = 0; i < borders.size(); ++i) {
            const RECT& rect = borders[i];
            m_borders[i] = CreateWindowExW(
                WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
                    WS_EX_LAYERED | WS_EX_TRANSPARENT,
                kBorderClassName, L"", WS_POPUP,
                rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (m_borders[i]) {
                SetLayeredWindowAttributes(m_borders[i], 0, 255, LWA_ALPHA);
                ShowWindow(m_borders[i], SW_SHOWNOACTIVATE);
            }
        }

        const UINT dpi = GetDpiForWindow(m_overlay);
        m_controlScale = dpi ? float(dpi) / 96.0f : 1.0f;
        // 提示使用独立的穿透窗口，以免遮罩的整体透明度使白色文字变灰。
        m_hintFont = CreateFontW(-ControlPixels(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei UI");
        m_hint = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE |
            WS_EX_LAYERED | WS_EX_TRANSPARENT, kHintClassName, L"", WS_POPUP,
            m_selection.x, m_selection.y, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!m_hint) {
            error = Tr(m_language, TextId::ScrollHintFailed);
            return false;
        }
        SetWindowLongPtrW(m_hint, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        SetLayeredWindowAttributes(m_hint, Colors::ToNativeRgb(Colors::Black), 255, LWA_COLORKEY);
        UpdateHint();
        const int controlWidth = ControlPixels(Toolbar::ScrollCaptureWidth);
        const int controlHeight = ControlPixels(Toolbar::ScrollCaptureHeight);
        const int gap = ControlPixels(8);
        int controlX = std::clamp(m_selection.x + m_selection.w - controlWidth,
                                  int(m_monitor.left) + gap,
                                  int(m_monitor.right) - controlWidth - gap);
        int controlY = m_selection.y + m_selection.h + gap;
        if (controlY + controlHeight > m_monitor.bottom - gap)
            controlY = m_selection.y - controlHeight - gap;
        controlY = std::clamp(controlY, int(m_monitor.top) + gap,
                              int(m_monitor.bottom) - controlHeight - gap);
        RECT controlRect{controlX, controlY, controlX + controlWidth, controlY + controlHeight};
        RECT selectionRect{m_selection.x, m_selection.y,
                           m_selection.x + m_selection.w, m_selection.y + m_selection.h};
        RECT overlap{};
        m_controlIntersectsSelection = IntersectRect(&overlap, &controlRect, &selectionRect) != FALSE;

        m_control = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
            kControlClassName, TrWide(m_language, TextId::ActionScroll).c_str(), WS_POPUP,
            controlX, controlY, controlWidth, controlHeight,
            nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!m_control) {
            error = Tr(m_language, TextId::ScrollControlsFailed);
            return false;
        }
        if (!UpdateControlBitmap(error)) return false;
        const int ids[] = {kEditButtonId, kSaveButtonId, kCancelButtonId, kFinishButtonId};
        const std::array<std::wstring, 4> titles = {TrWide(m_language, TextId::ScrollEdit), TrWide(m_language, TextId::ScrollSave), TrWide(m_language, TextId::ScrollCancel), TrWide(m_language, TextId::ActionCopyDone)};
        for (int i = 0; i < 4; ++i) {
            m_buttons[i] = CreateWindowExW(0, L"BUTTON", titles[i].c_str(),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | WS_TABSTOP,
                ControlPixels(8 + i * Toolbar::ScrollCaptureButtonStep), ControlPixels(Toolbar::ScrollCaptureButtonInsetY),
                ControlPixels(44), ControlPixels(Toolbar::ScrollCaptureButtonHeight), m_control,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(ids[i])),
                GetModuleHandleW(nullptr), nullptr);
        }
        ShowWindow(m_control, SW_SHOWNOACTIVATE);
        SetWindowPos(m_control, HWND_TOPMOST, controlX, controlY, controlWidth, controlHeight,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);

        const int leftSpace = m_selection.x - m_monitor.left - 16;
        const int rightSpace = m_monitor.right - (m_selection.x + m_selection.w) - 16;
        m_previewLeft = rightSpace < 180;
        m_previewMaxWidth = m_previewLeft ? std::clamp(leftSpace, 64, 180) : 180;
        m_previewMaxHeight = std::min(600, int(m_monitor.bottom - m_monitor.top - 16));
        m_previewX = m_previewLeft ? m_selection.x - 8 : m_selection.x + m_selection.w + 8;
        m_previewY = m_selection.y;
        // 左右空间都不足时限制在屏幕内；遮住选区的预览在采帧时隐藏。
        m_previewIntersectsSelection = m_previewLeft && leftSpace < m_previewMaxWidth;
        m_preview = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
            kPreviewClassName, L"", WS_POPUP,
            m_previewX, m_previewY, 1, 1, nullptr, nullptr,
            GetModuleHandleW(nullptr), this);

        return true;
    }

    void UpdateHint() {
        HDC dc = GetDC(nullptr);
        HGDIOBJ oldFont = SelectObject(dc, m_hintFont);
        RECT bounds{};
        DrawTextW(dc, TrWide(m_language, m_reverseScrollBlocked ? TextId::ScrollReverseHint : TextId::ScrollHint).c_str(),
                  -1, &bounds, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
        ReleaseDC(nullptr, dc);
        const int x = std::max(int(m_monitor.left),
            std::min(m_selection.x, int(m_monitor.right) - int(bounds.right)));
        int y = m_selection.y - ControlPixels(8) - bounds.bottom;
        m_hintIntersectsSelection = y < m_monitor.top;
        if (m_hintIntersectsSelection) {
            if (!m_reverseScrollBlocked) { ShowWindow(m_hint, SW_HIDE); return; }
            y = m_selection.y + ControlPixels(8);
        }
        SetWindowPos(m_hint, HWND_TOPMOST, x, y, bounds.right, bounds.bottom,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(m_hint, nullptr, TRUE);
    }

    void UpdatePreview(const ScrollImage& image) {
        if (!m_preview || m_previewMaxWidth <= 0 || m_previewMaxHeight <= 0 ||
            image.width <= 0 || image.height <= 0 || image.rgba.empty()) return;
        BITMAPINFO info{};
        info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth = image.width;
        info.bmiHeader.biHeight = -image.height;
        info.bmiHeader.biPlanes = 1;
        info.bmiHeader.biBitCount = 32;
        info.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!bitmap || !bits) {
            if (bitmap) DeleteObject(bitmap);
            return;
        }
        auto* destination = static_cast<uint8_t*>(bits);
        for (size_t i = 0; i < image.rgba.size() / 4; ++i) {
            destination[i * 4] = image.rgba[i * 4 + 2];
            destination[i * 4 + 1] = image.rgba[i * 4 + 1];
            destination[i * 4 + 2] = image.rgba[i * 4];
            destination[i * 4 + 3] = 255;
        }
        if (m_previewBitmap) DeleteObject(m_previewBitmap);
        m_previewBitmap = bitmap;
        m_previewImageWidth = image.width;
        m_previewImageHeight = image.height;
        const double ratio = std::min(double(m_previewMaxWidth) / image.width,
                                      double(m_previewMaxHeight) / image.height);
        const int width = std::max(1, int(std::round(image.width * ratio)));
        const int height = std::max(1, int(std::round(image.height * ratio)));
        const int x = std::clamp(m_previewLeft ? m_previewX - width : m_previewX, int(m_monitor.left) + 8,
                                 int(m_monitor.right) - width - 8);
        const int y = std::clamp(m_previewY, int(m_monitor.top) + 8,
                                 int(m_monitor.bottom) - height - 8);
        SetWindowPos(m_preview, HWND_TOPMOST, x, y, width, height,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(m_preview, nullptr, FALSE);
    }

    void PaintPreview(HWND window) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        if (m_previewBitmap) {
            HDC memoryDc = CreateCompatibleDC(dc);
            HGDIOBJ old = SelectObject(memoryDc, m_previewBitmap);
            SetStretchBltMode(dc, HALFTONE);
            StretchBlt(dc, 0, 0, client.right, client.bottom, memoryDc,
                       0, 0, m_previewImageWidth, m_previewImageHeight, SRCCOPY);
            SelectObject(memoryDc, old);
            DeleteDC(memoryDc);
        }
        HPEN pen = CreatePen(PS_SOLID, 1, SelectionBorderColor());
        HGDIOBJ oldPen = SelectObject(dc, pen);
        HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
        Rectangle(dc, 0, 0, client.right, client.bottom);
        SelectObject(dc, oldBrush);
        SelectObject(dc, oldPen);
        DeleteObject(pen);
        EndPaint(window, &paint);
    }

    bool UpdateControlBitmap(std::string& error) {
        RECT bounds{};
        GetWindowRect(m_control, &bounds);
        const auto toolbarBitmap = Toolbar::MakeScrollCaptureBitmap(m_controlScale, m_theme);
        BITMAPINFO toolbarInfo{};
        toolbarInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        toolbarInfo.bmiHeader.biWidth = toolbarBitmap.width;
        toolbarInfo.bmiHeader.biHeight = -toolbarBitmap.height;
        toolbarInfo.bmiHeader.biPlanes = 1;
        toolbarInfo.bmiHeader.biBitCount = 32;
        toolbarInfo.bmiHeader.biCompression = BI_RGB;
        void* toolbarBits = nullptr;
        HBITMAP bitmap = CreateDIBSection(nullptr, &toolbarInfo, DIB_RGB_COLORS,
                                           &toolbarBits, nullptr, 0);
        if (!bitmap || !toolbarBits) {
            if (bitmap) DeleteObject(bitmap);
            error = Tr(m_language, TextId::ScrollBitmapFailed);
            return false;
        }
        auto* pixels = static_cast<uint8_t*>(toolbarBits);
        for (size_t i = 0; i < toolbarBitmap.rgba.size() / 4; ++i) {
            // 分层窗口要求预乘 BGRA，保留圆角和细线的覆盖率。
            const unsigned alpha = toolbarBitmap.rgba[i * 4 + 3];
            pixels[i * 4] = (toolbarBitmap.rgba[i * 4 + 2] * alpha + 127) / 255;
            pixels[i * 4 + 1] = (toolbarBitmap.rgba[i * 4 + 1] * alpha + 127) / 255;
            pixels[i * 4 + 2] = (toolbarBitmap.rgba[i * 4] * alpha + 127) / 255;
            pixels[i * 4 + 3] = alpha;
        }
        HDC screenDc = GetDC(nullptr);
        HDC bitmapDc = CreateCompatibleDC(screenDc);
        HGDIOBJ oldBitmap = SelectObject(bitmapDc, bitmap);
        POINT destination{bounds.left, bounds.top};
        POINT source{0, 0};
        SIZE size{toolbarBitmap.width, toolbarBitmap.height};
        BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
        const BOOL updated = UpdateLayeredWindow(m_control, screenDc, &destination, &size,
                                                 bitmapDc, &source, 0, &blend, ULW_ALPHA);
        SelectObject(bitmapDc, oldBitmap);
        DeleteDC(bitmapDc);
        ReleaseDC(nullptr, screenDc);
        if (!updated) {
            DeleteObject(bitmap);
            error = Tr(m_language, TextId::ScrollBitmapShowFailed);
            return false;
        }
        if (m_controlBitmap) DeleteObject(m_controlBitmap);
        m_controlBitmap = bitmap;
        return true;
    }

    void DestroyAuxiliaryWindows() {
        if (m_hint) DestroyWindow(m_hint);
        m_hint = nullptr;
        if (m_hintFont) DeleteObject(m_hintFont);
        m_hintFont = nullptr;
        if (m_control) DestroyWindow(m_control);
        m_control = nullptr;
        if (m_controlBitmap) DeleteObject(m_controlBitmap);
        m_controlBitmap = nullptr;
        m_buttons.fill(nullptr);
        if (m_preview) DestroyWindow(m_preview);
        m_preview = nullptr;
        if (m_previewBitmap) DeleteObject(m_previewBitmap);
        m_previewBitmap = nullptr;
        for (HWND& mask : m_masks) {
            if (mask) DestroyWindow(mask);
            mask = nullptr;
        }
        for (HWND& border : m_borders) {
            if (border) DestroyWindow(border);
            border = nullptr;
        }
    }

    const Language m_language;
    Theme m_theme = Theme::Dark;
    ScrollCaptureController m_controller;
    HWND m_hint = nullptr;
    HFONT m_hintFont = nullptr;
    HWND m_overlay = nullptr;
    HWND m_control = nullptr;
    HBITMAP m_controlBitmap = nullptr;
    float m_controlScale = 1.0f;
    std::array<HWND, 4> m_buttons{};
    HWND m_preview = nullptr;
    HBITMAP m_previewBitmap = nullptr;
    std::array<HWND, 4> m_masks{};
    std::array<HWND, 4> m_borders{};
    GlobalRect m_selection;
    RECT m_monitor{};
    bool m_overlayWasVisible = false;
    bool m_controlIntersectsSelection = false;
    bool m_previewIntersectsSelection = false;
    bool m_hintIntersectsSelection = false;
    bool m_reverseScrollBlocked = false;
    bool m_previewLeft = false;
    int m_previewX = 0;
    int m_previewY = 0;
    int m_previewMaxWidth = 0;
    int m_previewMaxHeight = 0;
    int m_previewImageWidth = 0;
    int m_previewImageHeight = 0;
    ScrollCaptureAction m_action = ScrollCaptureAction::None;
    uint64_t m_epoch = 0;
    uint64_t m_revision = 0;
    std::shared_ptr<const ScrollImage> m_lastPreview;
};

HWND GetNativeWindow(SDL_Window* window) {
    if (!window) return nullptr;
    return static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
}

}  // namespace

std::unique_ptr<ScrollCaptureUI> ScrollCaptureUI::Create(
    SDL_Window* window, int, GlobalRect selection, double, std::string& error, Language language, Theme theme) {
    HWND overlay = GetNativeWindow(window);
    if (!overlay) {
        error = Tr(language, TextId::ScrollWindowMissing);
        return {};
    }
    if (selection.w <= 0 || selection.h <= 200) {
        error = Tr(language, TextId::ScrollTooSmall);
        return {};
    }
    RECT selectionRect{selection.x, selection.y,
                       selection.x + selection.w, selection.y + selection.h};
    HMONITOR monitor = MonitorFromRect(&selectionRect, MONITOR_DEFAULTTONULL);
    MONITORINFO info{sizeof(info)};
    if (!monitor || !GetMonitorInfoW(monitor, &info) ||
        selectionRect.left < info.rcMonitor.left || selectionRect.top < info.rcMonitor.top ||
        selectionRect.right > info.rcMonitor.right || selectionRect.bottom > info.rcMonitor.bottom) {
        error = Tr(language, TextId::ScrollSameDisplay);
        return {};
    }
    auto result = std::make_unique<UI>(overlay, selection, info.rcMonitor, language, theme);
    if (!result->Start(error)) return {};
    return result;
}

}  // namespace shotlet
