#include "NativeSystemTray.h"
#include "LocalizedText.h"
#include <SDL3/SDL.h>
#include <shellapi.h>
#include <memory>

namespace shotlet {
namespace {
constexpr wchar_t kTrayWindowClass[] = L"ShotletRecentTrayWindow";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kTrayIconId = 1;
constexpr UINT kFirstMenuCommand = 100;
constexpr int kBitmapChannels = 4;

HICON MakeIcon(SDL_Surface* surface) {
    BITMAPV5HEADER header{};
    header.bV5Size = sizeof(header);
    header.bV5Width = surface->w;
    header.bV5Height = -surface->h;
    header.bV5Planes = 1;
    header.bV5BitCount = 32;
    header.bV5Compression = BI_BITFIELDS;
    header.bV5RedMask = 0x00ff0000;
    header.bV5GreenMask = 0x0000ff00;
    header.bV5BlueMask = 0x000000ff;
    header.bV5AlphaMask = 0xff000000;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return nullptr;
    for (int y = 0; y < surface->h; ++y) {
        const auto* source = static_cast<const Uint8*>(surface->pixels) + y * surface->pitch;
        auto* destination = static_cast<Uint8*>(bits) + y * surface->w * kBitmapChannels;
        for (int x = 0; x < surface->w; ++x) {
            const int offset = x * kBitmapChannels;
            // Win32 图标采用预乘 BGRA，防止抗锯齿边缘产生黑边。
            const unsigned alpha = source[offset + 3];
            destination[offset] = source[offset + 2] * alpha / 255;
            destination[offset + 1] = source[offset + 1] * alpha / 255;
            destination[offset + 2] = source[offset] * alpha / 255;
            destination[offset + 3] = alpha;
        }
    }
    HBITMAP mask = CreateBitmap(surface->w, surface->h, 1, 1, nullptr);
    ICONINFO info{}; info.fIcon = TRUE; info.hbmColor = bitmap; info.hbmMask = mask;
    HICON result = mask ? CreateIconIndirect(&info) : nullptr;
    if (mask) DeleteObject(mask);
    DeleteObject(bitmap);
    return result;
}
} // namespace

struct NativeSystemTray {
    HWND window = nullptr;
    NOTIFYICONDATAW icon{};
    Language language;
    TrayActionCallback callback = nullptr;
    void* data = nullptr;
    UINT taskbarCreated = 0;
};
struct PopupFocusToken { HWND window = nullptr; };

namespace {
void ShowMenu(NativeSystemTray& tray) {
    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    const std::pair<TextId, TrayAction> entries[] = {
        {TextId::RecentCapturesTitle, TrayAction::Recent}, {TextId::CopyLatestCapture, TrayAction::CopyLatest},
        {TextId::HistoryTitle, TrayAction::History}, {TextId::SettingsTitle, TrayAction::Settings},
        {TextId::TrayQuit, TrayAction::Quit}};
    for (const auto& [label, action] : entries) {
        if (action == TrayAction::Settings) AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, kFirstMenuCommand + UINT(action), TrWide(tray.language, label).c_str());
    }
    POINT point{}; GetCursorPos(&point);
    const HWND previous = GetForegroundWindow();
    SetForegroundWindow(tray.window);
    const UINT command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, tray.window, nullptr);
    PostMessageW(tray.window, WM_NULL, 0, 0);
    DestroyMenu(menu);
    if (GetForegroundWindow() == tray.window && IsWindow(previous)) SetForegroundWindow(previous);
    if (command >= kFirstMenuCommand && command <= kFirstMenuCommand + UINT(TrayAction::Quit))
        tray.callback(tray.data, TrayAction(command - kFirstMenuCommand));
}

LRESULT CALLBACK TrayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* tray = reinterpret_cast<NativeSystemTray*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        tray = static_cast<NativeSystemTray*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(tray));
    }
    if (!tray) return DefWindowProcW(window, message, wParam, lParam);
    if (message == tray->taskbarCreated) {
        Shell_NotifyIconW(NIM_ADD, &tray->icon);
        Shell_NotifyIconW(NIM_SETVERSION, &tray->icon);
        return 0;
    }
    if (message == kTrayMessage) {
        const UINT action = LOWORD(lParam);
        if (action == NIN_SELECT || action == NIN_KEYSELECT) tray->callback(tray->data, TrayAction::Recent);
        else if (action == WM_CONTEXTMENU) ShowMenu(*tray);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
} // namespace

NativeSystemTray* CreateNativeSystemTray(SDL_Surface* icon, Language language, TrayActionCallback callback, void* data) {
    WNDCLASSW windowClass{};
    windowClass.lpfnWndProc = TrayWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kTrayWindowClass;
    if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    auto tray = std::make_unique<NativeSystemTray>();
    tray->language = language; tray->callback = callback; tray->data = data;
    tray->taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    tray->window = CreateWindowExW(WS_EX_TOOLWINDOW, kTrayWindowClass, L"", WS_POPUP, 0, 0, 0, 0,
                                    nullptr, nullptr, windowClass.hInstance, tray.get());
    if (!tray->window) return nullptr;
    tray->icon.cbSize = sizeof(tray->icon); tray->icon.hWnd = tray->window; tray->icon.uID = kTrayIconId;
    tray->icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    tray->icon.uCallbackMessage = kTrayMessage;
    tray->icon.hIcon = MakeIcon(icon);
    tray->icon.uVersion = NOTIFYICON_VERSION_4;
    const auto tooltip = TrWide(language, TextId::TrayTooltip);
    wcsncpy_s(tray->icon.szTip, tooltip.c_str(), _TRUNCATE);
    if (!tray->icon.hIcon || !Shell_NotifyIconW(NIM_ADD, &tray->icon)) {
        DestroyNativeSystemTray(tray.release()); return nullptr;
    }
    Shell_NotifyIconW(NIM_SETVERSION, &tray->icon);
    return tray.release();
}

void DestroyNativeSystemTray(NativeSystemTray* tray) {
    if (!tray) return;
    Shell_NotifyIconW(NIM_DELETE, &tray->icon);
    if (tray->window) DestroyWindow(tray->window);
    if (tray->icon.hIcon) DestroyIcon(tray->icon.hIcon);
    delete tray;
}

void UpdateNativeSystemTrayIcon(NativeSystemTray* tray, SDL_Surface* surface) {
    if (!tray) return;
    const HICON icon = MakeIcon(surface);
    if (!icon) return;
    const HICON previous = tray->icon.hIcon;
    tray->icon.hIcon = icon;
    Shell_NotifyIconW(NIM_MODIFY, &tray->icon);
    if (previous) DestroyIcon(previous);
}

SDL_Rect NativeSystemTrayAnchor(NativeSystemTray* tray) {
    NOTIFYICONIDENTIFIER identifier{}; identifier.cbSize = sizeof(identifier);
    identifier.hWnd = tray->window; identifier.uID = kTrayIconId;
    RECT bounds{};
    if (SUCCEEDED(Shell_NotifyIconGetRect(&identifier, &bounds)))
        return {bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top};
    POINT point{}; GetCursorPos(&point);
    return {point.x, point.y, 1, 1};
}

PopupFocusToken* RememberPopupFocus() { return new PopupFocusToken{GetForegroundWindow()}; }
void ReleasePopupFocus(PopupFocusToken* token, SDL_Window* popup, bool restore) {
    if (!token) return;
    const HWND handle = popup ? static_cast<HWND>(SDL_GetPointerProperty(SDL_GetWindowProperties(popup),
        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)) : nullptr;
    if (restore && (GetForegroundWindow() == handle || !GetForegroundWindow()) && IsWindow(token->window))
        SetForegroundWindow(token->window);
    delete token;
}
} // namespace shotlet
