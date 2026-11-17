#include "platform/LocalizedText.h"
#include "OcrWindow.h"
#include "platform/FontProvider.h"
#include "platform/OcrWindowLayout.h"

#if !defined(SHOTLET_PLATFORM_WINDOWS)
#error "OcrWindow_win.cpp must only be compiled on Windows"
#endif

#include "platform/PlatformWin32.h"
#include "platform/ClipboardService.h"
#include "utils/Colors.h"
#include "utils/Logger.h"
#include "utils/WindowChrome.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

namespace shotlet {
namespace {

static shotlet::Language s_language = shotlet::Language::English;
constexpr wchar_t kOcrWindowClassName[] = L"ShotletOcrWindow";
constexpr int kWindowBorderWidth = 1;
constexpr int kDividerWidth = 1;
constexpr float kTextPanelRatio = 0.5f;
// 标题栏高度与其他窗口共用统一常量；按钮宽度与置顶窗口、共享 ImGui 控件一致。
constexpr int kTitleBarHeight = static_cast<int>(shotlet::WindowChrome::TitleBarHeight);
constexpr int kTitleButtonWidth = 46;
constexpr int kTitleTextLeft = 12;

// 无边框窗口的自绘标题栏按钮状态。
enum class TitleButton {
    None,
    Minimize,
    Maximize,
    Close,
};
TitleButton s_hoverButton = TitleButton::None;
TitleButton s_pressedButton = TitleButton::None;
bool s_trackingMouseLeave = false;

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
constexpr DWORD DWMWA_USE_IMMERSIVE_DARK_MODE = 20;
#endif
#ifndef DWMWA_BORDER_COLOR
constexpr DWORD DWMWA_BORDER_COLOR = 34;
#endif
#ifndef DWMWA_CAPTION_COLOR
constexpr DWORD DWMWA_CAPTION_COLOR = 35;
#endif
#ifndef DWMWA_TEXT_COLOR
constexpr DWORD DWMWA_TEXT_COLOR = 36;
#endif

HWND s_hwnd = nullptr;
HWND s_editHwnd = nullptr;
HBITMAP s_bitmap = nullptr;
HFONT s_font = nullptr;
Theme s_theme = Theme::Dark;
std::unique_ptr<unsigned char[]> s_rgbaData;
int s_rgbaWidth = 0;
int s_rgbaHeight = 0;
std::string s_text;
bool s_classRegistered = false;
float s_windowScale = 1.0f;
float s_contentScaleFactor = 1.0f;
bool s_editVerticalScrollVisible = true;
bool s_loggedFirstPaint = false;
bool s_logNextPaintAfterResize = false;

HBRUSH s_backgroundBrush = nullptr;

COLORREF ToColorRef(ImU32 color) {
    return RGB((color >> IM_COL32_R_SHIFT) & 0xff,
               (color >> IM_COL32_G_SHIFT) & 0xff,
               (color >> IM_COL32_B_SHIFT) & 0xff);
}

float ClampScale(float scale) {
    if (!std::isfinite(scale)) return 1.0f;
    return std::clamp(scale, 1.0f, 3.0f);
}

int ScaleInt(int value) {
    return std::max(1, static_cast<int>(std::lround(static_cast<float>(value) * s_windowScale)));
}

RECT ClientRect(HWND hwnd) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    return rect;
}

int ImageDisplayWidth() {
    return std::max(1, static_cast<int>(
        std::lround(static_cast<float>(s_rgbaWidth) *
                    s_windowScale / std::max(0.01f, s_contentScaleFactor))));
}

int ImageDisplayHeight() {
    return std::max(1, static_cast<int>(
        std::lround(static_cast<float>(s_rgbaHeight) *
                    s_windowScale / std::max(0.01f, s_contentScaleFactor))));
}

void LogOcrScaleState(const char* reason) {
    if (!Logger::IsEnabled(LogLevel::Debug)) return;
    const float denominator = std::max(0.01f, s_contentScaleFactor);
    const float displayScale = s_windowScale / denominator;
    Logger::Debug(
        "[OcrWindow][scale-state] reason={}, imagePixels={}x{}, windowScale={:.3f}, contentScale={:.3f}, displayScale={:.3f}, imageDisplay={}x{}",
        reason,
        s_rgbaWidth,
        s_rgbaHeight,
        s_windowScale,
        s_contentScaleFactor,
        displayScale,
        ImageDisplayWidth(),
        ImageDisplayHeight());
}

// 无边框窗口：客户区包含自绘标题栏，内容区为标题栏以下的剩余部分。
bool ContainsPoint(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right &&
           point.y >= rect.top && point.y < rect.bottom;
}

RECT TitleBarRect(HWND hwnd) {
    RECT rect = ClientRect(hwnd);
    rect.bottom = std::min(rect.bottom, rect.top + ScaleInt(kTitleBarHeight));
    return rect;
}

OcrWindowLayout ContentLayout(HWND hwnd) {
    RECT client = ClientRect(hwnd);
    const RECT title = TitleBarRect(hwnd);
    const int titleBar = title.bottom - title.top;
    return CalculateOcrWindowLayout(client.right - client.left,
                                    std::max(1, static_cast<int>(client.bottom - client.top) - titleBar),
                                    ScaleInt(kDividerWidth),
                                    kTextPanelRatio);
}

RECT ImageRect(HWND hwnd) {
    RECT client = ClientRect(hwnd);
    const RECT title = TitleBarRect(hwnd);
    const int titleBar = title.bottom - title.top;
    auto layout = ContentLayout(hwnd);
    RECT rect;
    rect.left = client.left + layout.image.left;
    rect.top = client.top + titleBar + layout.image.top;
    rect.right = client.left + layout.image.right;
    rect.bottom = client.top + titleBar + layout.image.bottom;
    return rect;
}

RECT DividerRect(HWND hwnd) {
    RECT client = ClientRect(hwnd);
    const RECT title = TitleBarRect(hwnd);
    const int titleBar = title.bottom - title.top;
    auto layout = ContentLayout(hwnd);
    RECT rect;
    rect.left = client.left + layout.divider.left;
    rect.top = client.top + titleBar + layout.divider.top;
    rect.right = client.left + layout.divider.right;
    rect.bottom = client.top + titleBar + layout.divider.bottom;
    return rect;
}

RECT TextRect(HWND hwnd) {
    RECT client = ClientRect(hwnd);
    const RECT title = TitleBarRect(hwnd);
    const int titleBar = title.bottom - title.top;
    auto layout = ContentLayout(hwnd);
    RECT rect;
    rect.left = client.left + layout.text.left;
    rect.top = client.top + titleBar + layout.text.top;
    rect.right = client.left + layout.text.right;
    rect.bottom = client.top + titleBar + layout.text.bottom;
    return rect;
}

RECT ButtonRect(HWND hwnd, TitleButton button) {
    RECT title = TitleBarRect(hwnd);
    RECT rect = title;
    const int buttonWidth = ScaleInt(kTitleButtonWidth);

    switch (button) {
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

TitleButton HitTestTitleButton(HWND hwnd, POINT clientPoint) {
    if (ContainsPoint(ButtonRect(hwnd, TitleButton::Close), clientPoint)) return TitleButton::Close;
    if (ContainsPoint(ButtonRect(hwnd, TitleButton::Maximize), clientPoint)) return TitleButton::Maximize;
    if (ContainsPoint(ButtonRect(hwnd, TitleButton::Minimize), clientPoint)) return TitleButton::Minimize;
    return TitleButton::None;
}

void TrackMouseLeave(HWND hwnd) {
    if (s_trackingMouseLeave) return;
    TRACKMOUSEEVENT event{};
    event.cbSize = sizeof(TRACKMOUSEEVENT);
    event.dwFlags = TME_LEAVE;
    event.hwndTrack = hwnd;
    if (TrackMouseEvent(&event)) s_trackingMouseLeave = true;
}

void FillRectColor(HDC dc, const RECT& rect, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void DrawLine(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int width = 1) {
    HPEN pen = CreatePen(PS_SOLID, width, color);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

bool CreateDisplayBitmap(const unsigned char* rgbaData, int width, int height) {
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
        if (bitmap) DeleteObject(bitmap);
        Logger::Error("[OcrWindow] CreateDIBSection failed: {}", GetLastError());
        return false;
    }

    auto* bgra = static_cast<unsigned char*>(pixels);
    size_t pixelCount = static_cast<size_t>(width) * static_cast<size_t>(height);
    for (size_t i = 0; i < pixelCount; ++i) {
        bgra[i * 4 + 0] = rgbaData[i * 4 + 2];
        bgra[i * 4 + 1] = rgbaData[i * 4 + 1];
        bgra[i * 4 + 2] = rgbaData[i * 4 + 0];
        bgra[i * 4 + 3] = rgbaData[i * 4 + 3];
    }

    if (s_bitmap) DeleteObject(s_bitmap);
    s_bitmap = bitmap;
    return true;
}

void ResetState(bool destroyChildWindow) {
    if (s_backgroundBrush) DeleteObject(s_backgroundBrush);
    s_backgroundBrush = nullptr;
    if (s_bitmap) { DeleteObject(s_bitmap); s_bitmap = nullptr; }
    if (s_font) { DeleteObject(s_font); s_font = nullptr; }
    if (s_editHwnd) {
        HWND editHwnd = s_editHwnd;
        s_editHwnd = nullptr;
        if (destroyChildWindow && IsWindow(editHwnd)) {
            DestroyWindow(editHwnd);
        }
    }

    s_rgbaData.reset();
    s_rgbaWidth = 0;
    s_rgbaHeight = 0;
    s_text.clear();
    s_hwnd = nullptr;
    s_windowScale = 1.0f;
    s_contentScaleFactor = 1.0f;
    s_editVerticalScrollVisible = true;
    s_loggedFirstPaint = false;
    s_logNextPaintAfterResize = false;
    s_hoverButton = TitleButton::None;
    s_pressedButton = TitleButton::None;
    s_trackingMouseLeave = false;

}

void LogOcrWindowGeometry(const char* reason, HWND hwnd) {
    if (!hwnd || !Logger::IsEnabled(LogLevel::Debug)) return;

    RECT window{};
    RECT client{};
    GetWindowRect(hwnd, &window);
    GetClientRect(hwnd, &client);
    RECT image = ImageRect(hwnd);
    RECT text = TextRect(hwnd);

    Logger::Debug(
        "[OcrWindow][{}] window=({},{} {}x{}), client={}x{}, image=({},{} {}x{}), text=({},{} {}x{}), rgba={}x{}, imageDisplay={}x{}, windowScale={:.3f}, contentScale={:.3f}",
        reason,
        window.left,
        window.top,
        window.right - window.left,
        window.bottom - window.top,
        client.right - client.left,
        client.bottom - client.top,
        image.left,
        image.top,
        image.right - image.left,
        image.bottom - image.top,
        text.left,
        text.top,
        text.right - text.left,
        text.bottom - text.top,
        s_rgbaWidth,
        s_rgbaHeight,
        ImageDisplayWidth(),
        ImageDisplayHeight(),
        s_windowScale,
        s_contentScaleFactor);
}

void RequestClose(HWND hwnd) {
    if (!hwnd || hwnd != s_hwnd) return;

    ShowWindow(hwnd, SW_HIDE);
    if (s_editHwnd && IsWindow(s_editHwnd)) {
        ShowWindow(s_editHwnd, SW_HIDE);
    }
}

void ApplyOcrWindowTheme(HWND hwnd) {
    if (!hwnd) return;
    BOOL darkMode = s_theme == Theme::Dark;
    COLORREF captionColor = ToColorRef(GetThemePalette(s_theme).panelBackground);
    COLORREF borderColor = ToColorRef(GetThemePalette(s_theme).border);
    COLORREF textColor = ToColorRef(GetThemePalette(s_theme).text);

    HRESULT darkHr = DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    HRESULT captionHr = DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &captionColor, sizeof(captionColor));
    HRESULT borderHr = DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &borderColor, sizeof(borderColor));
    HRESULT textHr = DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &textColor, sizeof(textColor));
    // 圆角偏好与其他窗口共用统一入口，避免各窗口设置漂移。
    ApplyWindowCornerPreference(hwnd);
    Logger::Debug("[OcrWindow][theme] darkMode={}, darkHr=0x{:08x}, captionHr=0x{:08x}, borderHr=0x{:08x}, textHr=0x{:08x}",
                 darkMode ? 1 : 0,
                 static_cast<unsigned int>(darkHr),
                 static_cast<unsigned int>(captionHr),
                 static_cast<unsigned int>(borderHr),
                 static_cast<unsigned int>(textHr));
}

void ApplyEditTheme(HWND hwnd) {
    if (!hwnd) return;

    BOOL darkMode = s_theme == Theme::Dark;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));

    HMODULE uxTheme = LoadLibraryW(L"uxtheme.dll");
    if (!uxTheme) return;

    using SetWindowThemeFunc = HRESULT(WINAPI *)(HWND, LPCWSTR, LPCWSTR);
    auto setWindowTheme = reinterpret_cast<SetWindowThemeFunc>(
        GetProcAddress(uxTheme, "SetWindowTheme"));
    if (setWindowTheme) {
        setWindowTheme(hwnd, s_theme == Theme::Dark ? L"DarkMode_Explorer" : L"Explorer", nullptr);
    }

    FreeLibrary(uxTheme);
}

int EditLineHeight(HWND editHwnd) {
    HDC dc = GetDC(editHwnd);
    if (!dc) return 1;

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(editHwnd, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;

    TEXTMETRICW metrics{};
    int lineHeight = GetTextMetricsW(dc, &metrics) ? metrics.tmHeight : 1;

    if (oldFont) SelectObject(dc, oldFont);
    ReleaseDC(editHwnd, dc);
    return std::max(1, lineHeight);
}

bool EditNeedsVerticalScroll(HWND editHwnd) {
    if (!editHwnd) return false;

    int lineCount = static_cast<int>(SendMessageW(editHwnd, EM_GETLINECOUNT, 0, 0));
    if (lineCount <= 1) return false;

    RECT formatRect{};
    SendMessageW(editHwnd, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&formatRect));
    int formatHeight = formatRect.bottom - formatRect.top;
    if (formatHeight <= 0) {
        RECT client{};
        GetClientRect(editHwnd, &client);
        formatHeight = client.bottom - client.top;
    }

    int visibleLines = std::max(1, formatHeight / EditLineHeight(editHwnd));
    return lineCount > visibleLines;
}

void UpdateEditScrollBar() {
    if (!s_editHwnd) return;

    bool showVerticalScroll = EditNeedsVerticalScroll(s_editHwnd);
    if (showVerticalScroll == s_editVerticalScrollVisible) return;

    s_editVerticalScrollVisible = showVerticalScroll;
    ShowScrollBar(s_editHwnd, SB_VERT, showVerticalScroll ? TRUE : FALSE);
    RedrawWindow(s_editHwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME);
}

RECT ClampWindowRectToWorkArea(int x, int y, int width, int height) {
    RECT rect{x, y, x + width, y + height};
    HMONITOR monitor = MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO info{};
    info.cbSize = sizeof(MONITORINFO);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return rect;

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

void CreateEditControl(HWND parent) {
    RECT textArea = TextRect(parent);
    if (textArea.right <= textArea.left || textArea.bottom <= textArea.top) return;

    std::wstring wtext;
    if (s_text.empty()) {
        wtext = TrWide(s_language, TextId::OcrEmpty);
    } else {
        int len = MultiByteToWideChar(CP_UTF8, 0, s_text.c_str(), -1, nullptr, 0);
        if (len > 0) {
            wtext.resize(len - 1);
            MultiByteToWideChar(CP_UTF8, 0, s_text.c_str(), -1, wtext.data(), len);
        }
    }

    s_editHwnd = CreateWindowExW(
        0, L"EDIT", wtext.c_str(),
        WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_VSCROLL |
        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        textArea.left, textArea.top,
        textArea.right - textArea.left,
        textArea.bottom - textArea.top,
        parent, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!s_editHwnd) {
        Logger::Error("[OcrWindow] Create EDIT control failed: {}", GetLastError());
        return;
    }

    int fontSize = ScaleInt(static_cast<int>(std::lround(GetOcrFontSize())));
    s_font = CreateFontW(
        -fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_NATURAL_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Microsoft YaHei");

    SendMessageW(s_editHwnd, WM_SETFONT, reinterpret_cast<WPARAM>(s_font), TRUE);
    ApplyEditTheme(s_editHwnd);

    if (s_text.empty()) {
        SendMessageW(s_editHwnd, EM_SETSEL, 0, 0);
    }
    UpdateEditScrollBar();
}

void DrawTitleButtonIcon(HWND hwnd, HDC dc, TitleButton button, const RECT& rect) {
    const bool isPressed = s_pressedButton == button;
    const bool isHovered = s_hoverButton == button;

    if (isPressed || isHovered) {
        COLORREF bg = button == TitleButton::Close ? ToColorRef(Colors::NativeCloseHovered)
                                                   : ToColorRef(GetThemePalette(s_theme).hoverBackground);
        FillRectColor(dc, rect, bg);
    }

    COLORREF iconColor = ToColorRef(GetThemePalette(s_theme).secondaryText);
    if (button == TitleButton::Close && (isPressed || isHovered)) {
        iconColor = ToColorRef(GetThemePalette(s_theme).icon);
    }

    const int cx = (rect.left + rect.right) / 2;
    const int cy = (rect.top + rect.bottom) / 2 + 1;

    switch (button) {
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

void DrawTitleBar(HWND hwnd, HDC dc) {
    RECT title = TitleBarRect(hwnd);
    FillRectColor(dc, title, ToColorRef(GetThemePalette(s_theme).panelBackground));
    if (s_font) {
        HGDIOBJ oldFont = SelectObject(dc, s_font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ToColorRef(GetThemePalette(s_theme).text));
        RECT textRect = title;
        textRect.left += ScaleInt(kTitleTextLeft);
        // 标题文字与编辑控件共用字体，垂直居中于自绘标题栏。
        DrawTextW(dc, TrWide(s_language, TextId::OcrTitle).c_str(), -1, &textRect,
                  DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, oldFont);
    }
    DrawTitleButtonIcon(hwnd, dc, TitleButton::Minimize, ButtonRect(hwnd, TitleButton::Minimize));
    DrawTitleButtonIcon(hwnd, dc, TitleButton::Maximize, ButtonRect(hwnd, TitleButton::Maximize));
    DrawTitleButtonIcon(hwnd, dc, TitleButton::Close, ButtonRect(hwnd, TitleButton::Close));
}

void DrawWindowBorder(HWND hwnd, HDC dc) {
    RECT rect = ClientRect(hwnd);
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    HPEN pen = CreatePen(PS_SOLID, kWindowBorderWidth, ToColorRef(GetThemePalette(s_theme).border));
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void InvokeTitleButton(HWND hwnd, TitleButton button) {
    switch (button) {
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

LRESULT HitTestWindow(HWND hwnd, LPARAM lParam) {
    POINT screenPoint{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
    POINT clientPoint = screenPoint;
    ScreenToClient(hwnd, &clientPoint);
    if (HitTestTitleButton(hwnd, clientPoint) != TitleButton::None) {
        return HTCLIENT;
    }

    RECT windowRect{};
    GetWindowRect(hwnd, &windowRect);

    int frameX = GetSystemMetrics(SM_CXFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
    int frameY = GetSystemMetrics(SM_CYFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER);
    frameX = std::max(frameX, 6);
    frameY = std::max(frameY, 6);

    const bool left = screenPoint.x >= windowRect.left && screenPoint.x < windowRect.left + frameX;
    const bool right = screenPoint.x < windowRect.right && screenPoint.x >= windowRect.right - frameX;
    const bool top = screenPoint.y >= windowRect.top && screenPoint.y < windowRect.top + frameY;
    const bool bottom = screenPoint.y < windowRect.bottom && screenPoint.y >= windowRect.bottom - frameY;

    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;

    if (ContainsPoint(TitleBarRect(hwnd), clientPoint) &&
        HitTestTitleButton(hwnd, clientPoint) == TitleButton::None) {
        return HTCAPTION;
    }

    return HTCLIENT;
}

void PaintOcrWindow(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(hwnd, &ps);
    if (!dc) return;

    RECT client = ClientRect(hwnd);
    int clientWidth = client.right - client.left;
    int clientHeight = client.bottom - client.top;
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
    FillRectColor(paintDc, client, ToColorRef(GetThemePalette(s_theme).panelBackground));
    DrawTitleBar(hwnd, paintDc);

    if (s_bitmap && s_rgbaWidth > 0 && s_rgbaHeight > 0) {
        RECT imageRect = ImageRect(hwnd);
        if (!s_loggedFirstPaint || s_logNextPaintAfterResize) {
            Logger::Debug(
                "[OcrWindow][paint] reason={}, stretch dst=({},{} {}x{}), src={}x{}, client={}x{}, imageDisplay={}x{}, windowScale={:.3f}, contentScale={:.3f}",
                s_loggedFirstPaint ? "resize" : "initial",
                imageRect.left,
                imageRect.top,
                imageRect.right - imageRect.left,
                imageRect.bottom - imageRect.top,
                s_rgbaWidth,
                s_rgbaHeight,
                clientWidth,
                clientHeight,
                ImageDisplayWidth(),
                ImageDisplayHeight(),
                s_windowScale,
                s_contentScaleFactor);
            s_loggedFirstPaint = true;
            s_logNextPaintAfterResize = false;
        }
        HDC memDc = CreateCompatibleDC(paintDc);
        HGDIOBJ oldBitmap = SelectObject(memDc, s_bitmap);

        SetStretchBltMode(paintDc, HALFTONE);
        SetBrushOrgEx(paintDc, 0, 0, nullptr);
        StretchBlt(paintDc,
                   imageRect.left, imageRect.top,
                   imageRect.right - imageRect.left,
                   imageRect.bottom - imageRect.top,
                   memDc, 0, 0, s_rgbaWidth, s_rgbaHeight, SRCCOPY);

        SelectObject(memDc, oldBitmap);
        DeleteDC(memDc);
    }

    RECT imageRect = ImageRect(hwnd);
    HPEN imgPen = CreatePen(PS_SOLID, kWindowBorderWidth, ToColorRef(GetThemePalette(s_theme).border));
    HGDIOBJ oldImgPen = SelectObject(paintDc, imgPen);
    HGDIOBJ oldBrush = SelectObject(paintDc, GetStockObject(NULL_BRUSH));
    Rectangle(paintDc, imageRect.left, imageRect.top, imageRect.right, imageRect.bottom);
    SelectObject(paintDc, oldBrush);
    SelectObject(paintDc, oldImgPen);
    DeleteObject(imgPen);

    RECT divider = DividerRect(hwnd);
    FillRectColor(paintDc, divider, ToColorRef(GetThemePalette(s_theme).border));
    DrawWindowBorder(hwnd, paintDc);
    if (bufferDc && bufferBitmap) {
        BitBlt(dc, 0, 0, clientWidth, clientHeight, bufferDc, 0, 0, SRCCOPY);
        SelectObject(bufferDc, oldBufferBitmap);
        DeleteObject(bufferBitmap);
        DeleteDC(bufferDc);
    }

    EndPaint(hwnd, &ps);
}

void CopyOcrText() {
    if (!s_editHwnd) return;
    SendMessageW(s_editHwnd, EM_SETSEL, 0, -1);
    SendMessageW(s_editHwnd, WM_COPY, 0, 0);
    Logger::Info("[OcrWindow] OCR text copied to clipboard");
}

LRESULT CALLBACK OcrWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            CreateEditControl(hwnd);
            return 0;

        case WM_PAINT:
            PaintOcrWindow(hwnd);
            return 0;

        case WM_ERASEBKGND:
            return 1;

        // 无边框窗口：客户区覆盖整个窗口，标题栏与边缘缩放由自绘和命中测试提供。
        case WM_NCCALCSIZE:
            return 0;

        case WM_NCPAINT:
            return 0;

        case WM_NCHITTEST:
            return HitTestWindow(hwnd, lParam);

        case WM_NCLBUTTONDBLCLK:
            if (wParam == HTCAPTION) {
                InvokeTitleButton(hwnd, TitleButton::Maximize);
                return 0;
            }
            break;

        case WM_NCACTIVATE:
            InvalidateRect(hwnd, nullptr, FALSE);
            break;

        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            TitleButton button = HitTestTitleButton(hwnd, point);
            if (button != TitleButton::None) {
                s_pressedButton = button;
                s_hoverButton = button;
                SetCapture(hwnd);
                const RECT invalid = ButtonRect(hwnd, button);
                InvalidateRect(hwnd, &invalid, FALSE);
                return 0;
            }
            break;
        }

        case WM_LBUTTONUP: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            TitleButton hitButton = HitTestTitleButton(hwnd, point);
            TitleButton pressedButton = s_pressedButton;
            if (s_pressedButton != TitleButton::None) {
                ReleaseCapture();
                s_pressedButton = TitleButton::None;
                InvalidateRect(hwnd, nullptr, FALSE);
                if (pressedButton == hitButton) {
                    InvokeTitleButton(hwnd, hitButton);
                }
                return 0;
            }
            break;
        }

        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            TitleButton button = HitTestTitleButton(hwnd, point);
            if (button != s_hoverButton) {
                s_hoverButton = button;
                const RECT invalid = TitleBarRect(hwnd);
                InvalidateRect(hwnd, &invalid, FALSE);
            }
            TrackMouseLeave(hwnd);
            return 0;
        }

        case WM_MOUSELEAVE:
            s_trackingMouseLeave = false;
            if (s_hoverButton != TitleButton::None) {
                s_hoverButton = TitleButton::None;
                const RECT invalid = TitleBarRect(hwnd);
                InvalidateRect(hwnd, &invalid, FALSE);
            }
            return 0;

        case WM_SIZE:
            Logger::Debug("[OcrWindow][WM_SIZE] wParam={}, width={}, height={}",
                         static_cast<unsigned int>(wParam),
                         LOWORD(lParam),
                         HIWORD(lParam));
            s_logNextPaintAfterResize = true;
            if (s_editHwnd) {
                RECT textArea = TextRect(hwnd);
                MoveWindow(s_editHwnd, textArea.left, textArea.top,
                           textArea.right - textArea.left,
                           textArea.bottom - textArea.top, FALSE);
                UpdateEditScrollBar();
                RedrawWindow(s_editHwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE);
            }
            LogOcrWindowGeometry("after-WM_SIZE", hwnd);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;

        case WM_LBUTTONDBLCLK: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (ContainsPoint(ImageRect(hwnd), point)) {
                if (s_rgbaData && s_rgbaWidth > 0 && s_rgbaHeight > 0) {
                    CopyImageToClipboard(s_rgbaData.get(), s_rgbaWidth, s_rgbaHeight);
                    Logger::Info("[OcrWindow] Image copied to clipboard");
                }
            }
            return 0;
        }

        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORSTATIC: {
            HDC editDc = reinterpret_cast<HDC>(wParam);
            SetBkMode(editDc, OPAQUE);
            SetBkColor(editDc, ToColorRef(GetThemePalette(s_theme).panelBackground));
            SetTextColor(editDc, s_text.empty() ? ToColorRef(GetThemePalette(s_theme).secondaryText) : ToColorRef(GetThemePalette(s_theme).text));
            if (!s_backgroundBrush) s_backgroundBrush = CreateSolidBrush(ToColorRef(GetThemePalette(s_theme).panelBackground));
            return reinterpret_cast<LRESULT>(s_backgroundBrush);
        }

        case WM_CLOSE:
            RequestClose(hwnd);
            return 0;

        case WM_DESTROY:
            if (hwnd == s_hwnd) {
                Logger::Info("[OcrWindow] Window destroyed");
                ResetState(false);
            }
            return 0;

        default:
            break;
    }

    return DefWindowProcW(hwnd, message, wParam, lParam);
}

bool RegisterOcrWindowClass() {
    if (s_classRegistered) return true;

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = OcrWindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = nullptr;
    wc.hIconSm = nullptr;
    wc.lpszClassName = kOcrWindowClassName;

    ATOM atom = RegisterClassExW(&wc);
    if (!atom) {
        DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            Logger::Error("[OcrWindow] RegisterClassExW failed: {}", error);
            return false;
        }
    }

    s_classRegistered = true;
    return true;
}

}

bool OcrWindow::Create(const unsigned char* rgbaData, int width, int height,
                       const std::string& text,
                       float screenX, float screenY,
                       float scaleFactor, Language language, Theme theme) {
    if (!rgbaData || width <= 0 || height <= 0) {
        Logger::Error("[OcrWindow] Invalid parameters");
        return false;
    }
    s_language = language;
    s_theme = theme;

    if (s_hwnd) {
        Destroy();
        ProcessEvents();
    }

    if (!RegisterOcrWindowClass()) {
        return false;
    }
    size_t dataSize = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    s_rgbaData = std::make_unique<unsigned char[]>(dataSize);
    std::memcpy(s_rgbaData.get(), rgbaData, dataSize);
    s_rgbaWidth = width;
    s_rgbaHeight = height;
    s_text = text;
    s_contentScaleFactor = std::isfinite(scaleFactor) && scaleFactor > 0.0f ? scaleFactor : 1.0f;

    if (!CreateDisplayBitmap(s_rgbaData.get(), width, height)) {
        ResetState(false);
        return false;
    }

    s_windowScale = ClampScale(s_contentScaleFactor);
    int divider = ScaleInt(kDividerWidth);
    int imageDisplayW = ImageDisplayWidth();
    int imageDisplayH = ImageDisplayHeight();
    int textPanelW = static_cast<int>(std::lround(static_cast<float>(imageDisplayW) * kTextPanelRatio));
    int clientW = imageDisplayW + divider + textPanelW;
    int clientH = imageDisplayH;

    int windowX = static_cast<int>(std::lround(screenX));
    int windowY = static_cast<int>(std::lround(screenY));

    // 无边框窗口：WM_NCCALCSIZE 返回 0，窗口尺寸即客户区尺寸（含自绘标题栏）。
    DWORD style = WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_CLIPCHILDREN;
    DWORD exStyle = WS_EX_TOPMOST;
    const int titleBar = ScaleInt(kTitleBarHeight);
    const int windowW = clientW;
    const int windowH = titleBar + clientH;
    // 图片内容保持在请求的屏幕位置，标题栏从其上方展开。
    RECT clamped = ClampWindowRectToWorkArea(windowX, windowY - titleBar, windowW, windowH);

    Logger::Debug(
        "[OcrWindow][create-input] rgba={}x{}, textLength={}, screen=({:.0f},{:.0f}), windowScale={:.3f}, contentScale={:.3f}, displayScale={:.3f}, style=0x{:08x}, exStyle=0x{:08x}",
        width,
        height,
        text.size(),
        screenX,
        screenY,
        s_windowScale,
        s_contentScaleFactor,
        s_windowScale / std::max(0.01f, s_contentScaleFactor),
        static_cast<unsigned int>(style),
        static_cast<unsigned int>(exStyle));
    Logger::Debug(
        "[OcrWindow][create-layout] client={}x{} imageDisplay={}x{} imagePixels={}x{} divider={} textPanel={} titleBar={} window={}x{}, requested=({},{}), clamped=({},{} {}x{})",
        clientW,
        clientH,
        imageDisplayW,
        imageDisplayH,
        width,
        height,
        divider,
        textPanelW,
        titleBar,
        windowW,
        windowH,
        windowX,
        windowY - titleBar,
        clamped.left,
        clamped.top,
        clamped.right - clamped.left,
        clamped.bottom - clamped.top);

    s_hwnd = CreateWindowExW(
        exStyle,
        kOcrWindowClassName,
        TrWide(s_language, TextId::OcrTitle).c_str(),
        style,
        clamped.left, clamped.top,
        clamped.right - clamped.left,
        clamped.bottom - clamped.top,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);

    if (!s_hwnd) {
        Logger::Error("[OcrWindow] CreateWindowExW failed: {}", GetLastError());
        ResetState(false);
        return false;
    }

    ApplyOcrWindowTheme(s_hwnd);
    LogOcrScaleState("after-CreateWindow");
    LogOcrWindowGeometry("after-CreateWindow", s_hwnd);
    ShowWindow(s_hwnd, SW_SHOWNORMAL);
    SetWindowPos(s_hwnd, HWND_TOPMOST, clamped.left, clamped.top,
                 clamped.right - clamped.left, clamped.bottom - clamped.top,
                 SWP_SHOWWINDOW);
    LogOcrWindowGeometry("after-SetWindowPos", s_hwnd);
    SetForegroundWindow(s_hwnd);
    UpdateWindow(s_hwnd);

    Logger::Info("[OcrWindow] Created: {}x{} image, text length={}, at ({},{})",
                 width, height, text.size(), clamped.left, clamped.top);
    return true;
}

void OcrWindow::SetTheme(Theme theme) {
    s_theme = theme;
    if (!s_hwnd) return;
    if (s_backgroundBrush) DeleteObject(s_backgroundBrush);
    s_backgroundBrush = CreateSolidBrush(ToColorRef(GetThemePalette(theme).panelBackground));
    ApplyOcrWindowTheme(s_hwnd);
    ApplyEditTheme(s_editHwnd);
    RedrawWindow(s_hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}

void OcrWindow::Destroy() {
    if (s_hwnd) {
        HWND hwnd = s_hwnd;
        DestroyWindow(hwnd);
    }
}

bool OcrWindow::IsVisible() {
    return s_hwnd && IsWindow(s_hwnd) && IsWindowVisible(s_hwnd);
}

void OcrWindow::ProcessEvents() {
    if (!s_hwnd) return;

    MSG msg{};
    while (PeekMessageW(&msg, s_hwnd, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (s_editHwnd) {
        while (PeekMessageW(&msg, s_editHwnd, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

}
