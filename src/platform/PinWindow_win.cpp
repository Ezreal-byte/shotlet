#include "platform/LocalizedText.h"
#include "PinWindow.h"
#include "platform/ClipboardService.h"
#include "platform/FileService.h"

#if !defined(SHOTLET_PLATFORM_WINDOWS)
#error "PinWindow_win.cpp must only be compiled on Windows"
#endif

#include "platform/PlatformWin32.h"
#include "utils/Colors.h"
#include "utils/Logger.h"
#include "utils/WindowChrome.h"

#include <dwmapi.h>
#include <objidl.h>
#include <gdiplus.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace shotlet {
namespace {

struct PinPoint {
    float x;
    float y;
};

constexpr wchar_t kPinWindowClassName[] = L"ShotletPinWindow";
constexpr UINT kCommandCopy = 1001;
constexpr UINT kCommandSave = 1002;
constexpr int kTitleBarHeight = static_cast<int>(WindowChrome::TitleBarHeight);
constexpr int kTitlePinButtonWidth = 44;
constexpr int kTitleButtonWidth = 46;
constexpr int kWindowBorderWidth = 1;

#ifndef DWMWA_BORDER_COLOR
constexpr DWORD DWMWA_BORDER_COLOR = 34;
#endif
#ifndef DWMWA_CAPTION_COLOR
constexpr DWORD DWMWA_CAPTION_COLOR = 35;
#endif
#ifndef DWMWA_TEXT_COLOR
constexpr DWORD DWMWA_TEXT_COLOR = 36;
#endif
#ifndef DWMWA_NCRENDERING_POLICY
constexpr DWORD DWMWA_NCRENDERING_POLICY = 2;
#endif
enum DWMNCRENDERINGPOLICY {
    DWMNCRP_USEWINDOWSTYLE = 0,
    DWMNCRP_DISABLED = 1,
    DWMNCRP_ENABLED = 2,
    DWMNCRP_LAST = 3,
};

bool s_classRegistered = false;

enum class TitleButton {
    None,
    Pin,
    Minimize,
    Maximize,
    Close,
};

struct PinWindowState : std::enable_shared_from_this<PinWindowState> {
    HWND hwnd = nullptr;
    HBITMAP bitmap = nullptr;
    Language language = Language::English;
    Theme theme = Theme::Dark;
    std::unique_ptr<unsigned char[]> rgbaData;
    int rgbaWidth = 0;
    int rgbaHeight = 0;
    bool trackingMouseLeave = false;
    float windowScale = 1.0f;
    TitleButton hoverButton = TitleButton::None;
    TitleButton pressedButton = TitleButton::None;
    bool pinActive = true;

    ~PinWindowState() {
        if (bitmap)
            DeleteObject(bitmap);
    }
};

std::vector<std::shared_ptr<PinWindowState>> s_pinWindows;

void RequestClose(HWND hwnd) {
    if (hwnd)
        DestroyWindow(hwnd);
}

bool CreateDisplayBitmap(PinWindowState &state, const unsigned char *rgbaData, int width, int height) {
    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HDC screenDc = GetDC(nullptr);
    HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &pixels, nullptr, 0);
    ReleaseDC(nullptr, screenDc);

    if (!bitmap || !pixels) {
        if (bitmap) {
            DeleteObject(bitmap);
        }
        Logger::Error("[PinWindow] CreateDIBSection failed: {}", GetLastError());
        return false;
    }

    auto* bgra = static_cast<unsigned char*>(pixels);
    const size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < pixelCount; ++i) {
        bgra[i * 4 + 0] = rgbaData[i * 4 + 2];
        bgra[i * 4 + 1] = rgbaData[i * 4 + 1];
        bgra[i * 4 + 2] = rgbaData[i * 4 + 0];
        bgra[i * 4 + 3] = rgbaData[i * 4 + 3];
    }

    if (state.bitmap) {
        DeleteObject(state.bitmap);
    }
    state.bitmap = bitmap;
    return true;
}

float ClampScale(float scale) {
    if (!std::isfinite(scale)) {
        return 1.0f;
    }
    return std::clamp(scale, 1.0f, 3.0f);
}

int ScaleInt(const PinWindowState &state, int value) {
    return std::max(1, static_cast<int>(std::lround(static_cast<float>(value) * state.windowScale)));
}

PinPoint PinPointTowards(const PinPoint from, const PinPoint to, const float distance) {
    const float dx = to.x - from.x;
    const float dy = to.y - from.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0f) {
        return from;
    }

    const float t = std::min(distance / len, 0.45f);
    return {from.x + dx * t, from.y + dy * t};
}

POINT ToPoint(const PinPoint point) {
    return {static_cast<LONG>(std::lround(point.x)), static_cast<LONG>(std::lround(point.y))};
}

void QuadraticCurveTo(HDC dc, const PinPoint start, const PinPoint control, const PinPoint end) {
    const PinPoint control1{
        start.x + (control.x - start.x) * 2.0f / 3.0f,
        start.y + (control.y - start.y) * 2.0f / 3.0f,
    };
    const PinPoint control2{
        end.x + (control.x - end.x) * 2.0f / 3.0f,
        end.y + (control.y - end.y) * 2.0f / 3.0f,
    };
    POINT curve[] = {ToPoint(control1), ToPoint(control2), ToPoint(end)};
    PolyBezierTo(dc, curve, static_cast<DWORD>(std::size(curve)));
}

Gdiplus::PointF ToGdiPoint(const PinPoint point) {
    return {point.x, point.y};
}

void AddQuadraticCurve(Gdiplus::GraphicsPath& path,
                       const PinPoint start,
                       const PinPoint control,
                       const PinPoint end) {
    const PinPoint control1{
        start.x + (control.x - start.x) * 2.0f / 3.0f,
        start.y + (control.y - start.y) * 2.0f / 3.0f,
    };
    const PinPoint control2{
        end.x + (control.x - end.x) * 2.0f / 3.0f,
        end.y + (control.y - end.y) * 2.0f / 3.0f,
    };
    path.AddBezier(ToGdiPoint(start), ToGdiPoint(control1), ToGdiPoint(control2), ToGdiPoint(end));
}

class GdiplusSession {
public:
    GdiplusSession() {
        Gdiplus::GdiplusStartupInput input;
        if (Gdiplus::GdiplusStartup(&token_, &input, nullptr) == Gdiplus::Ok) {
            initialized_ = true;
        }
    }

    ~GdiplusSession() {
        if (initialized_) {
            Gdiplus::GdiplusShutdown(token_);
        }
    }

    bool IsInitialized() const {
        return initialized_;
    }

private:
    ULONG_PTR token_ = 0;
    bool initialized_ = false;
};

bool IsGdiplusAvailable() {
    static GdiplusSession session;
    return session.IsInitialized();
}

COLORREF ToColorRef(ImU32 color) {
    return RGB((color >> IM_COL32_R_SHIFT) & 0xff,
               (color >> IM_COL32_G_SHIFT) & 0xff,
               (color >> IM_COL32_B_SHIFT) & 0xff);
}

RECT ClientRect(HWND hwnd) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    return rect;
}

RECT TitleBarRect(const PinWindowState &state, HWND hwnd) {
    RECT rect = ClientRect(hwnd);
    rect.bottom = std::min(rect.bottom, rect.top + ScaleInt(state, kTitleBarHeight));
    return rect;
}

RECT ImageRect(const PinWindowState &state, HWND hwnd) {
    RECT rect = ClientRect(hwnd);
    rect.top = std::min(rect.bottom, rect.top + ScaleInt(state, kTitleBarHeight));
    return rect;
}

RECT ButtonRect(const PinWindowState &state, HWND hwnd, TitleButton button) {
    RECT title = TitleBarRect(state, hwnd);
    RECT rect = title;
    const int pinButtonWidth = ScaleInt(state, kTitlePinButtonWidth);
    const int buttonWidth = ScaleInt(state, kTitleButtonWidth);

    switch (button) {
        case TitleButton::Pin:
            rect.left = title.left;
            rect.right = std::min(title.right, title.left + pinButtonWidth);
            break;
        case TitleButton::Close:
            rect.left = std::max(title.left, title.right - buttonWidth);
            rect.right = title.right;
            break;
        case TitleButton::Maximize:
            rect.left = std::max(title.left, title.right - buttonWidth * 2);
            rect.right = std::max(rect.left, title.right - buttonWidth);
            break;
        case TitleButton::Minimize:
            rect.left = std::max(title.left, title.right - buttonWidth * 3);
            rect.right = std::max(rect.left, title.right - buttonWidth * 2);
            break;
        case TitleButton::None:
            rect = RECT{0, 0, 0, 0};
            break;
    }

    return rect;
}

bool ContainsPoint(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right &&
           point.y >= rect.top && point.y < rect.bottom;
}

TitleButton HitTestTitleButton(const PinWindowState &state, HWND hwnd, POINT clientPoint) {
    if (ContainsPoint(ButtonRect(state, hwnd, TitleButton::Pin), clientPoint)) {
        return TitleButton::Pin;
    }
    if (ContainsPoint(ButtonRect(state, hwnd, TitleButton::Close), clientPoint)) {
        return TitleButton::Close;
    }
    if (ContainsPoint(ButtonRect(state, hwnd, TitleButton::Maximize), clientPoint)) {
        return TitleButton::Maximize;
    }
    if (ContainsPoint(ButtonRect(state, hwnd, TitleButton::Minimize), clientPoint)) {
        return TitleButton::Minimize;
    }
    return TitleButton::None;
}

bool IsTitleButton(TitleButton button) {
    return button != TitleButton::None;
}

void TrackMouseLeave(PinWindowState &state, HWND hwnd) {
    if (state.trackingMouseLeave) {
        return;
    }

    TRACKMOUSEEVENT event{};
    event.cbSize = sizeof(TRACKMOUSEEVENT);
    event.dwFlags = TME_LEAVE;
    event.hwndTrack = hwnd;
    if (TrackMouseEvent(&event)) {
        state.trackingMouseLeave = true;
    }
}

void InvalidateRectValue(HWND hwnd, RECT rect, BOOL erase = FALSE) {
    InvalidateRect(hwnd, &rect, erase);
}

COLORREF PinWindowBorderColor(const PinWindowState &state) { return ToColorRef(GetThemePalette(state.theme).border); }

void ApplyPinWindowTheme(const PinWindowState &state, HWND hwnd) {
    if (!hwnd) {
        return;
    }

    COLORREF captionColor = ToColorRef(GetThemePalette(state.theme).panelBackground);
    COLORREF borderColor = PinWindowBorderColor(state);
    COLORREF textColor = ToColorRef(GetThemePalette(state.theme).icon);
    DWMNCRENDERINGPOLICY ncPolicy = DWMNCRP_ENABLED;

    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));
    DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &ncPolicy, sizeof(ncPolicy));
    // 圆角偏好与其余窗口共用统一入口。
    ApplyWindowCornerPreference(hwnd);
}

void ApplyPinTopmostState(const PinWindowState &state) {
    const HWND insertAfter = state.pinActive ? HWND_TOPMOST : HWND_NOTOPMOST;
    if (state.hwnd) {
        SetWindowPos(state.hwnd, insertAfter, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    }
}

void DrawLine(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int width = 1) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawToolbarPinIcon(const PinWindowState &state, HDC dc, const RECT &buttonRect, bool active) {
    const int iconSize = ScaleInt(state, 20);
    const int iconLeft = buttonRect.left + (buttonRect.right - buttonRect.left - iconSize) / 2;
    const int iconTop = buttonRect.top + (buttonRect.bottom - buttonRect.top - iconSize) / 2;
    const auto iconSizeF = static_cast<float>(iconSize);
    const float s = iconSizeF * 0.68f;
    const float ox = static_cast<float>(iconLeft) + iconSizeF * 0.16f;
    const float oy = static_cast<float>(iconTop) + iconSizeF * 0.16f;

    COLORREF color = active ? ToColorRef(GetThemePalette(state.theme).pinActive)
                            : ToColorRef(GetThemePalette(state.theme).secondaryText);
    if (IsGdiplusAvailable()) {
        Gdiplus::Graphics graphics(dc);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        Gdiplus::Color penColor(255, GetRValue(color), GetGValue(color), GetBValue(color));
        Gdiplus::Pen pen(penColor, static_cast<Gdiplus::REAL>(std::max(1, ScaleInt(state, 1))));
        pen.SetLineJoin(Gdiplus::LineJoinRound);
        pen.SetStartCap(Gdiplus::LineCapRound);
        pen.SetEndCap(Gdiplus::LineCapRound);

        const PinPoint p0{ox + s * 0.25f, oy + s * 0.00f};
        const PinPoint p1{ox + s * 0.75f, oy + s * 0.00f};
        const PinPoint p2{ox + s * 0.62f, oy + s * 0.25f};
        const PinPoint p3{ox + s * 0.90f, oy + s * 0.70f};
        const PinPoint p4{ox + s * 0.10f, oy + s * 0.70f};
        const PinPoint p5{ox + s * 0.38f, oy + s * 0.25f};
        const float cornerRadius = s * 0.08f;
        const PinPoint p0Out = PinPointTowards(p0, p1, cornerRadius);
        const PinPoint p1In = PinPointTowards(p1, p0, cornerRadius);
        const PinPoint p1Out = PinPointTowards(p1, p2, cornerRadius);
        const PinPoint p3In = PinPointTowards(p3, p2, cornerRadius);
        const PinPoint p3Out = PinPointTowards(p3, p4, cornerRadius);
        const PinPoint p4In = PinPointTowards(p4, p3, cornerRadius);
        const PinPoint p4Out = PinPointTowards(p4, p5, cornerRadius);
        const PinPoint p0In = PinPointTowards(p0, p5, cornerRadius);

        Gdiplus::GraphicsPath headPath;
        headPath.StartFigure();
        headPath.AddLine(ToGdiPoint(p0Out), ToGdiPoint(p1In));
        AddQuadraticCurve(headPath, p1In, p1, p1Out);
        headPath.AddLine(ToGdiPoint(p1Out), ToGdiPoint(p2));
        headPath.AddLine(ToGdiPoint(p2), ToGdiPoint(p3In));
        AddQuadraticCurve(headPath, p3In, p3, p3Out);
        headPath.AddLine(ToGdiPoint(p3Out), ToGdiPoint(p4In));
        AddQuadraticCurve(headPath, p4In, p4, p4Out);
        headPath.AddLine(ToGdiPoint(p4Out), ToGdiPoint(p5));
        headPath.AddLine(ToGdiPoint(p5), ToGdiPoint(p0In));
        AddQuadraticCurve(headPath, p0In, p0, p0Out);
        graphics.DrawPath(&pen, &headPath);

        const float pinX = ox + s * 0.50f;
        graphics.DrawLine(&pen,
                          Gdiplus::PointF(pinX, oy + s * 0.70f),
                          Gdiplus::PointF(pinX, oy + s * 1.00f));
        return;
    }

    HPEN pen = CreatePen(PS_SOLID, ScaleInt(state, 1), color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));

    const PinPoint p0{ox + s * 0.25f, oy + s * 0.00f};
    const PinPoint p1{ox + s * 0.75f, oy + s * 0.00f};
    const PinPoint p2{ox + s * 0.62f, oy + s * 0.25f};
    const PinPoint p3{ox + s * 0.90f, oy + s * 0.70f};
    const PinPoint p4{ox + s * 0.10f, oy + s * 0.70f};
    const PinPoint p5{ox + s * 0.38f, oy + s * 0.25f};
    const float cornerRadius = s * 0.08f;
    const PinPoint p0Out = PinPointTowards(p0, p1, cornerRadius);
    const PinPoint p1In = PinPointTowards(p1, p0, cornerRadius);
    const PinPoint p1Out = PinPointTowards(p1, p2, cornerRadius);
    const PinPoint p3In = PinPointTowards(p3, p2, cornerRadius);
    const PinPoint p3Out = PinPointTowards(p3, p4, cornerRadius);
    const PinPoint p4In = PinPointTowards(p4, p3, cornerRadius);
    const PinPoint p4Out = PinPointTowards(p4, p5, cornerRadius);
    const PinPoint p0In = PinPointTowards(p0, p5, cornerRadius);

    MoveToEx(dc, ToPoint(p0Out).x, ToPoint(p0Out).y, nullptr);
    LineTo(dc, ToPoint(p1In).x, ToPoint(p1In).y);
    QuadraticCurveTo(dc, p1In, p1, p1Out);
    LineTo(dc, ToPoint(p2).x, ToPoint(p2).y);
    LineTo(dc, ToPoint(p3In).x, ToPoint(p3In).y);
    QuadraticCurveTo(dc, p3In, p3, p3Out);
    LineTo(dc, ToPoint(p4In).x, ToPoint(p4In).y);
    QuadraticCurveTo(dc, p4In, p4, p4Out);
    LineTo(dc, ToPoint(p5).x, ToPoint(p5).y);
    LineTo(dc, ToPoint(p0In).x, ToPoint(p0In).y);
    QuadraticCurveTo(dc, p0In, p0, p0Out);

    const int pinX = static_cast<int>(std::lround(ox + s * 0.50f));
    MoveToEx(dc, pinX, static_cast<int>(std::lround(oy + s * 0.70f)), nullptr);
    LineTo(dc, pinX, static_cast<int>(std::lround(oy + s * 1.00f)));

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void FillRectColor(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void DrawTitleButtonIcon(const PinWindowState &state, HWND hwnd, HDC dc, TitleButton button, const RECT &rect) {
    const bool isPressed = state.pressedButton == button;
    const bool isHovered = state.hoverButton == button;

    if (isPressed || isHovered) {
        COLORREF bg = button == TitleButton::Close ? ToColorRef(Colors::NativeCloseHovered)
                                                   : ToColorRef(GetThemePalette(state.theme).hoverBackground);
        FillRectColor(dc, rect, bg);
    }

    COLORREF iconColor = ToColorRef(GetThemePalette(state.theme).secondaryText);
    if (button == TitleButton::Close && (isPressed || isHovered)) {
        iconColor = ToColorRef(GetThemePalette(state.theme).icon);
    }

    int cx = (rect.left + rect.right) / 2;
    int cy = (rect.top + rect.bottom) / 2 + 1;

    switch (button) {
        case TitleButton::Pin:
            DrawToolbarPinIcon(state, dc, rect, state.pinActive);
            break;
        case TitleButton::Minimize:
            DrawLine(dc, cx - 7, cy + 2, cx + 7, cy + 2, iconColor, 1);
            break;
        case TitleButton::Maximize:
            if (IsZoomed(hwnd)) {
                HPEN pen = CreatePen(PS_SOLID, 1, iconColor);
                HGDIOBJ oldPen = SelectObject(dc, pen);
                HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, cx - 5, cy - 5, cx + 6, cy + 6);
                Rectangle(dc, cx - 8, cy - 2, cx + 3, cy + 9);
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(pen);
            } else {
                HPEN pen = CreatePen(PS_SOLID, 1, iconColor);
                HGDIOBJ oldPen = SelectObject(dc, pen);
                HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
                Rectangle(dc, cx - 7, cy - 7, cx + 8, cy + 8);
                SelectObject(dc, oldBrush);
                SelectObject(dc, oldPen);
                DeleteObject(pen);
            }
            break;
        case TitleButton::Close:
            DrawLine(dc, cx - 7, cy - 7, cx + 7, cy + 7, iconColor, 1);
            DrawLine(dc, cx + 7, cy - 7, cx - 7, cy + 7, iconColor, 1);
            break;
        case TitleButton::None:
            break;
    }
}

void DrawTitleBar(const PinWindowState &state, HWND hwnd, HDC dc) {
    RECT title = TitleBarRect(state, hwnd);
    FillRectColor(dc, title, ToColorRef(GetThemePalette(state.theme).panelBackground));
    DrawTitleButtonIcon(state, hwnd, dc, TitleButton::Pin, ButtonRect(state, hwnd, TitleButton::Pin));
    DrawTitleButtonIcon(state, hwnd, dc, TitleButton::Minimize, ButtonRect(state, hwnd, TitleButton::Minimize));
    DrawTitleButtonIcon(state, hwnd, dc, TitleButton::Maximize, ButtonRect(state, hwnd, TitleButton::Maximize));
    DrawTitleButtonIcon(state, hwnd, dc, TitleButton::Close, ButtonRect(state, hwnd, TitleButton::Close));
}

void DrawWindowBorder(const PinWindowState &state, HWND hwnd, HDC dc) {
    RECT rect = ClientRect(hwnd);
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }

    HPEN pen = CreatePen(PS_SOLID, kWindowBorderWidth, PinWindowBorderColor(state));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));

    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void DrawImageBorder(const PinWindowState &state, HWND hwnd, HDC dc) {
    RECT rect = ImageRect(state, hwnd);
    if (rect.right <= rect.left || rect.bottom <= rect.top) {
        return;
    }

    HPEN pen = CreatePen(PS_SOLID, kWindowBorderWidth, PinWindowBorderColor(state));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));

    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);

    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void CopyPinnedImage(const PinWindowState &state) {
    if (!state.rgbaData || state.rgbaWidth <= 0 || state.rgbaHeight <= 0) {
        return;
    }

    if (CopyImageToClipboard(state.rgbaData.get(), state.rgbaWidth, state.rgbaHeight)) {
        Logger::Info("[PinWindow] Image copied to clipboard");
    }
}

void SavePinnedImage(const PinWindowState &state, HWND owner) {
    if (!state.rgbaData || state.rgbaWidth <= 0 || state.rgbaHeight <= 0) {
        return;
    }

    SetForegroundWindow(owner);
    std::string path = SaveFileWithDialog(state.rgbaData.get(), state.rgbaWidth, state.rgbaHeight, state.language);
    if (!path.empty()) {
        Logger::Info("[PinWindow] event=image.saved");
    }

    if (state.hwnd) {
        ApplyPinTopmostState(state);
    }
}

void ShowContextMenu(const PinWindowState &state, HWND hwnd, POINT screenPoint) {
    HMENU menu = CreatePopupMenu();
    if (!menu) {
        return;
    }

    AppendMenuW(menu, MF_STRING, kCommandCopy, TrWide(state.language, TextId::CopyClipboard).c_str());
    AppendMenuW(menu, MF_STRING, kCommandSave, TrWide(state.language, TextId::SaveFile).c_str());

    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_TOPALIGN | TPM_LEFTALIGN,
                   screenPoint.x, screenPoint.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

void PaintPinnedImage(const PinWindowState &state, HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    if (!dc) {
        return;
    }

    RECT client = ClientRect(hwnd);
    const int clientWidth = client.right - client.left;
    const int clientHeight = client.bottom - client.top;
    if (clientWidth <= 0 || clientHeight <= 0) {
        EndPaint(hwnd, &ps);
        return;
    }

    HDC paintDc = dc;
    HDC bufferDc = CreateCompatibleDC(dc);
    HBITMAP bufferBitmap = nullptr;
    HGDIOBJ oldBufferBitmap = nullptr;
    if (bufferDc) {
        bufferBitmap = CreateCompatibleBitmap(dc, clientWidth, clientHeight);
        if (bufferBitmap) {
            oldBufferBitmap = SelectObject(bufferDc, bufferBitmap);
            paintDc = bufferDc;
        } else {
            DeleteDC(bufferDc);
            bufferDc = nullptr;
        }
    }

    HBRUSH bgBrush = CreateSolidBrush(ToColorRef(Colors::PinInitialBackground));
    FillRect(paintDc, &client, bgBrush);
    DeleteObject(bgBrush);
    DrawTitleBar(state, hwnd, paintDc);

    if (state.bitmap && state.rgbaWidth > 0 && state.rgbaHeight > 0) {
        RECT imageRect = ImageRect(state, hwnd);
        HDC memDc = CreateCompatibleDC(paintDc);
        HGDIOBJ oldBitmap = SelectObject(memDc, state.bitmap);

        SetStretchBltMode(paintDc, HALFTONE);
        SetBrushOrgEx(paintDc, 0, 0, nullptr);
        StretchBlt(paintDc, imageRect.left, imageRect.top, imageRect.right - imageRect.left,
                   imageRect.bottom - imageRect.top, memDc, 0, 0, state.rgbaWidth, state.rgbaHeight, SRCCOPY);

        SelectObject(memDc, oldBitmap);
        DeleteDC(memDc);
    }

    DrawImageBorder(state, hwnd, paintDc);
    DrawWindowBorder(state, hwnd, paintDc);

    if (bufferDc && bufferBitmap) {
        BitBlt(dc, 0, 0, clientWidth, clientHeight, bufferDc, 0, 0, SRCCOPY);
        SelectObject(bufferDc, oldBufferBitmap);
        DeleteObject(bufferBitmap);
        DeleteDC(bufferDc);
    }

    EndPaint(hwnd, &ps);
}

void InvokeTitleButton(PinWindowState &state, HWND hwnd, TitleButton button) {
    switch (button) {
        case TitleButton::Pin:
            state.pinActive = !state.pinActive;
            ApplyPinTopmostState(state);
            InvalidateRectValue(hwnd, TitleBarRect(state, hwnd));
            Logger::Info("[PinWindow] Topmost {}", state.pinActive ? "enabled" : "disabled");
            break;
        case TitleButton::Minimize:
            ShowWindow(hwnd, SW_MINIMIZE);
            break;
        case TitleButton::Maximize:
            ShowWindow(hwnd, IsZoomed(hwnd) ? SW_RESTORE : SW_MAXIMIZE);
            InvalidateRect(hwnd, nullptr, TRUE);
            break;
        case TitleButton::Close:
            RequestClose(hwnd);
            break;
        case TitleButton::None:
            break;
    }
}

LRESULT HitTestWindow(const PinWindowState &state, HWND hwnd, LPARAM lParam) {
    POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    POINT clientPoint = screenPoint;
    ScreenToClient(hwnd, &clientPoint);
    if (IsTitleButton(HitTestTitleButton(state, hwnd, clientPoint))) {
        return HTCLIENT;
    }

    RECT windowRect{};
    GetWindowRect(hwnd, &windowRect);

    int frameX = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
    int frameY = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
    frameX = std::max(frameX, 6);
    frameY = std::max(frameY, 6);

    bool left = screenPoint.x >= windowRect.left && screenPoint.x < windowRect.left + frameX;
    bool right = screenPoint.x < windowRect.right && screenPoint.x >= windowRect.right - frameX;
    bool top = screenPoint.y >= windowRect.top && screenPoint.y < windowRect.top + frameY;
    bool bottom = screenPoint.y < windowRect.bottom && screenPoint.y >= windowRect.bottom - frameY;

    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;

    if (ContainsPoint(TitleBarRect(state, hwnd), clientPoint) &&
        !IsTitleButton(HitTestTitleButton(state, hwnd, clientPoint))) {
        return HTCAPTION;
    }

    return HTCLIENT;
}

void ShowImageContextMenu(const PinWindowState &state, HWND hwnd, LPARAM lParam) {
    POINT point{};
    if (GET_X_LPARAM(lParam) == -1 && GET_Y_LPARAM(lParam) == -1) {
        RECT rect = ImageRect(state, hwnd);
        point.x = rect.left + 16;
        point.y = rect.top + 16;
        ClientToScreen(hwnd, &point);
    } else {
        point.x = GET_X_LPARAM(lParam);
        point.y = GET_Y_LPARAM(lParam);
    }

    POINT clientPoint = point;
    ScreenToClient(hwnd, &clientPoint);
    if (!ContainsPoint(ImageRect(state, hwnd), clientPoint)) {
        return;
    }

    ShowContextMenu(state, hwnd, point);
}

void HandleDpiChanged(PinWindowState &state, HWND hwnd, WPARAM wParam, LPARAM lParam) {
    const UINT dpiX = LOWORD(wParam);
    if (dpiX > 0) {
        state.windowScale = ClampScale(static_cast<float>(dpiX) / 96.0f);
    }

    const auto* suggestedRect = reinterpret_cast<const RECT*>(lParam);
    if (suggestedRect) {
        SetWindowPos(hwnd,
                     nullptr,
                     suggestedRect->left,
                     suggestedRect->top,
                     suggestedRect->right - suggestedRect->left,
                     suggestedRect->bottom - suggestedRect->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
    }

    InvalidateRect(hwnd, nullptr, TRUE);
}

LRESULT CALLBACK PinWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto *windowState = reinterpret_cast<PinWindowState *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lParam);
        windowState = static_cast<PinWindowState *>(create->lpCreateParams);
        windowState->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(windowState));
    }
    if (!windowState)
        return DefWindowProcW(hwnd, message, wParam, lParam);
    // 文件对话框和菜单会运行嵌套消息循环，整个回调期间必须保留图片及按钮状态。
    const auto retainedState = windowState->shared_from_this();
    auto &state = *retainedState;
    switch (message) {
        case WM_CREATE:
            return 0;

        case WM_PAINT:
            PaintPinnedImage(state, hwnd);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        case WM_NCCALCSIZE:
            return 0;

        case WM_NCACTIVATE:
            InvalidateRect(hwnd, nullptr, FALSE);
            return TRUE;

        case WM_NCPAINT:
            return 0;

        case WM_SIZE:
            InvalidateRect(hwnd, nullptr, TRUE);
            return 0;

        case WM_DPICHANGED:
            HandleDpiChanged(state, hwnd, wParam, lParam);
            return 0;

        case WM_NCHITTEST:
            return HitTestWindow(state, hwnd, lParam);

        case WM_NCLBUTTONDBLCLK:
            if (wParam == HTCAPTION) {
                InvokeTitleButton(state, hwnd, TitleButton::Maximize);
                return 0;
            }
            break;

        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            TitleButton button = HitTestTitleButton(state, hwnd, point);
            if (IsTitleButton(button)) {
                state.pressedButton = button;
                state.hoverButton = button;
                SetCapture(hwnd);
                InvalidateRectValue(hwnd, ButtonRect(state, hwnd, button));
                return 0;
            }
            break;
        }

        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            TitleButton hitButton = HitTestTitleButton(state, hwnd, point);
            TitleButton pressedButton = state.pressedButton;
            if (state.pressedButton != TitleButton::None) {
                ReleaseCapture();
                state.pressedButton = TitleButton::None;
                InvalidateRect(hwnd, nullptr, FALSE);
                if (pressedButton == hitButton) {
                    InvokeTitleButton(state, hwnd, hitButton);
                }
                return 0;
            }
            break;
        }

        case WM_LBUTTONDBLCLK: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (ContainsPoint(ImageRect(state, hwnd), point)) {
                CopyPinnedImage(state);
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            TitleButton button = HitTestTitleButton(state, hwnd, point);
            if (button != state.hoverButton) {
                state.hoverButton = button;
                InvalidateRectValue(hwnd, TitleBarRect(state, hwnd));
            }
            TrackMouseLeave(state, hwnd);
            return 0;
        }

        case WM_MOUSELEAVE:
            state.trackingMouseLeave = false;
            if (state.hoverButton != TitleButton::None) {
                state.hoverButton = TitleButton::None;
                InvalidateRectValue(hwnd, TitleBarRect(state, hwnd));
            }
            return 0;

        case WM_CONTEXTMENU: {
            ShowImageContextMenu(state, hwnd, lParam);
            return 0;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case kCommandCopy:
                    CopyPinnedImage(state);
                    return 0;
                case kCommandSave:
                    SavePinnedImage(state, hwnd);
                    return 0;
                default:
                    break;
            }
            break;

        case WM_CLOSE:
            RequestClose(hwnd);
            return 0;

        case WM_NCDESTROY:
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            state.hwnd = nullptr;
            Logger::Info("[PinWindow] Window destroyed");
            break;

        default:
            break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool RegisterPinWindowClass() {
    if (s_classRegistered) {
        return true;
    }

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc = PinWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = kPinWindowClassName;

    ATOM atom = RegisterClassExW(&wc);
    if (!atom) {
        DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            Logger::Error("[PinWindow] RegisterClassExW failed: {}", error);
            return false;
        }
    }

    s_classRegistered = true;
    return true;
}

RECT ClampWindowRectToWorkArea(int x, int y, int width, int height) {
    RECT rect{x, y, x + width, y + height};

    HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(MONITORINFO);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) {
        return rect;
    }

    const int minVisible = 64;
    int rectWidth = rect.right - rect.left;
    int rectHeight = rect.bottom - rect.top;
    RECT work = info.rcWork;

    if (rectWidth > work.right - work.left) {
        rect.left = work.left;
        rect.right = work.right;
    } else {
        if (rect.right < work.left + minVisible) {
            rect.right = work.left + minVisible;
            rect.left = rect.right - rectWidth;
        }
        if (rect.left > work.right - minVisible) {
            rect.left = work.right - minVisible;
            rect.right = rect.left + rectWidth;
        }
    }

    if (rectHeight > work.bottom - work.top) {
        rect.top = work.top;
        rect.bottom = work.bottom;
    } else {
        if (rect.bottom < work.top + minVisible) {
            rect.bottom = work.top + minVisible;
            rect.top = rect.bottom - rectHeight;
        }
        if (rect.top > work.bottom - minVisible) {
            rect.top = work.bottom - minVisible;
            rect.bottom = rect.top + rectHeight;
        }
    }

    return rect;
}

} // namespace

bool PinWindow::Create(const unsigned char* rgbaData, int width, int height,
                       float screenX, float screenY,
                       float scaleFactor, Language language, Theme theme) {
    if (!rgbaData || width <= 0 || height <= 0) {
        Logger::Error("[PinWindow] Invalid parameters: data={}, size={}x{}",
                      rgbaData != nullptr, width, height);
        return false;
    }
    if (!RegisterPinWindowClass()) {
        return false;
    }

    const auto windowState = std::make_shared<PinWindowState>();
    auto &state = *windowState;
    state.language = language;
    state.theme = theme;
    state.pinActive = true;

    constexpr size_t rgbaBytesPerPixel = 4;
    const size_t dataSize = static_cast<size_t>(width) * static_cast<size_t>(height) * rgbaBytesPerPixel;
    state.rgbaData = std::make_unique<unsigned char[]>(dataSize);
    std::memcpy(state.rgbaData.get(), rgbaData, dataSize);
    state.rgbaWidth = width;
    state.rgbaHeight = height;

    if (!CreateDisplayBitmap(state, state.rgbaData.get(), width, height)) {
        return false;
    }

    DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX;
    DWORD exStyle = WS_EX_TOPMOST | WS_EX_TOOLWINDOW;
    int clientWidth = width;
    state.windowScale = ClampScale(scaleFactor);
    const int titleBarHeight = ScaleInt(state, kTitleBarHeight);
    int clientHeight = height + titleBarHeight;
    int windowX = static_cast<int>(std::lround(screenX));
    int windowY = static_cast<int>(std::lround(screenY)) - titleBarHeight;

    RECT rect{
        static_cast<LONG>(windowX),
        static_cast<LONG>(windowY),
        static_cast<LONG>(windowX) + clientWidth,
        static_cast<LONG>(windowY) + clientHeight
    };
    RECT clamped = ClampWindowRectToWorkArea(rect.left, rect.top,
                                             rect.right - rect.left,
                                             rect.bottom - rect.top);

    state.hwnd = CreateWindowExW(exStyle, kPinWindowClassName, L"", style, clamped.left, clamped.top,
                                 clamped.right - clamped.left, clamped.bottom - clamped.top, nullptr, nullptr,
                                 GetModuleHandleW(nullptr), windowState.get());

    if (!state.hwnd) {
        DWORD error = GetLastError();
        Logger::Error("[PinWindow] CreateWindowExW failed: {}", error);
        return false;
    }

    s_pinWindows.push_back(windowState);
    ApplyPinWindowTheme(state, state.hwnd);
    ShowWindow(state.hwnd, SW_SHOWNORMAL);
    SetWindowPos(state.hwnd, HWND_TOPMOST, clamped.left, clamped.top, clamped.right - clamped.left,
                 clamped.bottom - clamped.top, SWP_SHOWWINDOW);
    SetForegroundWindow(state.hwnd);
    UpdateWindow(state.hwnd);

    Logger::Info("[PinWindow] Created: {}x{} at ({},{}), imageTopLeft=({:.0f},{:.0f}), scale={:.2f}, topmost", width,
                 height, clamped.left, clamped.top, screenX, screenY, state.windowScale);
    return true;
}

void PinWindow::SetTheme(Theme theme) {
    const auto windows = s_pinWindows;
    for (const auto &state: windows) {
        state->theme = theme;
        if (!state->hwnd)
            continue;
        ApplyPinWindowTheme(*state, state->hwnd);
        RedrawWindow(state->hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    }
}

void PinWindow::Destroy() {
    const auto windows = std::move(s_pinWindows);
    s_pinWindows.clear();
    for (const auto &state: windows)
        RequestClose(state->hwnd);
}

bool PinWindow::IsVisible() {
    // 最小化后 HWND 仍有效，最后一个窗口真正销毁之前都应保持进程存活。
    return std::any_of(s_pinWindows.begin(), s_pinWindows.end(),
                       [](const auto &state) { return state->hwnd != nullptr; });
}

void PinWindow::ProcessEvents() {
    // 分别处理每个原生窗口的消息，不取走 SDL 或 OCR 窗口的消息。
    const auto windows = s_pinWindows;
    for (const auto &state: windows) {
        MSG msg{};
        while (state->hwnd && PeekMessageW(&msg, state->hwnd, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    // 销毁消息只使自身失效；退出回调后再释放资源，避免在消息处理期间悬空。
    std::erase_if(s_pinWindows, [](const auto &state) { return state->hwnd == nullptr; });
}

} // namespace shotlet
