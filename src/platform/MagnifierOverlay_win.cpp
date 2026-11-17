#include "utils/Colors.h"
#include "platform/LocalizedText.h"
#include "MagnifierOverlay.h"
#include "utils/Logger.h"

#if !defined(SHOTLET_PLATFORM_WINDOWS)
#error "MagnifierOverlay_win.cpp must only be compiled on Windows"
#endif

#include "platform/PlatformWin32.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <memory>
#include <cstdio>
#include <cstdarg>

using namespace shotlet;

// 放大镜参数（与 Mac 版布局一致）
static const int kMagnifierSize = 128;   // 放大区域宽度/高度
static const int kInfoHeight    = 78;    // 信息区高度
static const int kTotalHeight   = kMagnifierSize + kInfoHeight;  // 总窗口高度
static const int kMagPixels     = 15;
static const int kMagOffset     = 16;
static const int kCornerRadius  = 8;

static float ClampUIScale(float scale) {
    if (!std::isfinite(scale)) return 1.0f;
    return std::clamp(scale, 1.0f, 3.0f);
}

static int ScaleInt(int value, float scale) {
    return std::max(1, static_cast<int>(std::lround(static_cast<float>(value) * scale)));
}

static void ApplyRoundedCornersAndBorder(uint32_t* bits, int width, int height, int radius) {
    if (!bits || width <= 0 || height <= 0 || radius <= 0) return;

    const float inset = 0.5f;
    const float left = inset;
    const float top = inset;
    const float right = static_cast<float>(width) - 1.0f - inset;
    const float bottom = static_cast<float>(height) - 1.0f - inset;
    const float r = static_cast<float>(std::min(radius, std::min(width, height) / 2));
    const uint32_t borderColor = Colors::ToBGRA(Colors::MagnifierPremultipliedBorder);

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const float px = static_cast<float>(x) + 0.5f;
            const float py = static_cast<float>(y) + 0.5f;

            const float cx = std::clamp(px, left + r, right - r);
            const float cy = std::clamp(py, top + r, bottom - r);
            const float dx = px - cx;
            const float dy = py - cy;
            const float cornerDistance = std::sqrt(dx * dx + dy * dy);

            bool outside = false;
            if ((px < left + r || px > right - r) &&
                (py < top + r || py > bottom - r) &&
                cornerDistance > r) {
                outside = true;
            }

            uint32_t& pixel = bits[y * width + x];
            if (outside) {
                pixel = 0;
                continue;
            }

            const float edgeDistance = std::min(
                std::min(px - left, right - px),
                std::min(py - top, bottom - py));
            const bool nearStraightEdge = edgeDistance < 1.0f;
            const bool nearCornerEdge =
                (px < left + r || px > right - r) &&
                (py < top + r || py > bottom - r) &&
                cornerDistance >= r - 1.0f;

            if (nearStraightEdge || nearCornerEdge) {
                pixel = borderColor;
            }
        }
    }
}

static void DrawCrosshair(uint32_t* bits, int width, int height, int centerX, int centerY,
                          int thickness, uint32_t color) {
    if (!bits || width <= 0 || height <= 0 || thickness <= 0) return;

    const int halfLower = thickness / 2;
    const int halfUpper = thickness - halfLower - 1;

    for (int dx = 0; dx < width; dx++) {
        for (int dy = -halfLower; dy <= halfUpper; dy++) {
            int y = centerY + dy;
            if (y >= 0 && y < height) {
                bits[y * width + dx] = color;
            }
        }
    }

    for (int dy = 0; dy < height; dy++) {
        for (int dx = -halfLower; dx <= halfUpper; dx++) {
            int x = centerX + dx;
            if (x >= 0 && x < width) {
                bits[dy * width + x] = color;
            }
        }
    }
}

static const wchar_t* kMagnifierClassName = L"ShotletMagnifier";
static bool s_classRegistered = false;

// Forward declaration
static LRESULT CALLBACK MagnifierWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam);

struct shotlet::MagnifierOverlay::Impl {
    HWND hwnd = nullptr;
    HDC hdcMem = nullptr;
    HBITMAP hBitmap = nullptr;
    uint32_t* bitmapBits = nullptr; // DIB section pixel pointer
    HFONT hFont = nullptr;          // 信息区字体
    HFONT hSmallFont = nullptr;     // 快捷键提示字体
    shotlet::MagnifierOverlay* owner = nullptr;
};

// ─── Find the ScreenData index for a given screen coordinate ───
static int FindScreenIndexForPoint(const std::vector<shotlet::ScreenData>& screens,
                                   int pointX, int pointY) {
    for (size_t i = 0; i < screens.size(); i++) {
        const auto& screen = screens[i];
        int hitW = screen.pixelWidth;
        int hitH = screen.pixelHeight;
        if (pointX >= screen.screenX && pointX < screen.screenX + hitW &&
            pointY >= screen.screenY && pointY < screen.screenY + hitH) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

// ─── 安全格式化文字到宽字符缓冲区 ───
static void FormatMagnifierText(wchar_t* buf, int bufSize, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    // 先格式化到 char 缓冲区
    char tmp[256];
    vsnprintf(tmp, sizeof(tmp), fmt, args);
    va_end(args);
    // 转换为宽字符
    MultiByteToWideChar(CP_UTF8, 0, tmp, -1, buf, bufSize);
}

// ─── 绘制一行文字到 DIB section ───
static void DrawTextLine(HDC hdc, HFONT hFont, uint32_t* bits,
                         int width, int totalHeight,
                         const wchar_t* text, int x, int y, int lineHeight,
                         COLORREF textColor, COLORREF bgColor) {
    SelectObject(hdc, hFont);
    SetTextColor(hdc, textColor);
    SetBkMode(hdc, TRANSPARENT);

    RECT rc = {x, y, width - x, y + lineHeight};
    DrawTextW(hdc, text, -1, &rc, DT_LEFT | DT_SINGLELINE | DT_NOCLIP);

    // GDI DrawText 在 32-bit DIB 上留下 alpha=0，
    // 需要手动修复信息区的 alpha 通道为 255（全不透明）
    for (int row = y; row < std::min(y + lineHeight, totalHeight); row++) {
        for (int col = 0; col < width; col++) {
            uint32_t& pixel = bits[row * width + col];
            uint8_t a = (pixel >> 24) & 0xFF;
            if (a == 0) {
                // GDI 没设 alpha → 设为 255
                pixel |= (255u << 24);
            }
        }
    }
}

// ─── Render magnifier content ───
static void RenderMagnifier(shotlet::MagnifierOverlay* overlay) {
    if (!overlay || !overlay->m_impl || !overlay->m_impl->hwnd) return;
    if (overlay->m_screens.empty()) return;

    auto* impl = overlay->m_impl.get();
    const auto& palette = shotlet::GetThemePalette(overlay->m_theme);
    auto nativeColor = [](ImU32 c) { return RGB((c >> IM_COL32_R_SHIFT) & 255, (c >> IM_COL32_G_SHIFT) & 255, (c >> IM_COL32_B_SHIFT) & 255); };
    auto bitmapColor = [](ImU32 c) { return Colors::ToBGRA(c | IM_COL32_A_MASK); };
    if (!impl->hdcMem || !impl->bitmapBits) return;
    const float uiScale = ClampUIScale(overlay->GetUIScale());
    const int magnifierSize = ScaleInt(kMagnifierSize, uiScale);
    const int infoHeight = ScaleInt(kInfoHeight, uiScale);
    const int totalHeight = magnifierSize + infoHeight;
    const int magPixelsPx = ScaleInt(kMagPixels, uiScale);
    const int magOffset = ScaleInt(kMagOffset, uiScale);
    const int lineHeight = std::max(20, ScaleInt(20, uiScale));
    const int textX = ScaleInt(6, uiScale);
    const int cornerRadius = ScaleInt(kCornerRadius, uiScale);
    const int crossThickness = std::max(2, static_cast<int>(std::lround(1.8f * uiScale)));
    int mouseX = overlay->m_mouseX;
    int mouseY = overlay->m_mouseY;

    // Find the screen containing the mouse
    int screenIdx = FindScreenIndexForPoint(overlay->m_screens, mouseX, mouseY);
    if (screenIdx < 0) screenIdx = 0;

    const auto& screen = overlay->m_screens[screenIdx];
    const uint8_t* screenData = screen.GetData();
    if (!screenData || screen.pixelWidth <= 0 || screen.pixelHeight <= 0) return;

    float sf = screen.scaleFactor;

    // Windows V2: mouseX/Y and screenX/Y are both physical pixels
    int localX = mouseX - screen.screenX;
    int localY = mouseY - screen.screenY;

    // Capture area size in pixels
    int captureSizePx = 2 * magPixelsPx + 1;

    // Sample origin (clamped to screen bounds)
    int magX = std::max(0, std::min(localX - magPixelsPx,
                                     screen.pixelWidth - captureSizePx));
    int magY = std::max(0, std::min(localY - magPixelsPx,
                                     screen.pixelHeight - captureSizePx));

    // ── 放大区域：最近邻放大到 kMagnifierSize × kMagnifierSize ──
    uint32_t bgBlack  = Colors::ToBGRA(Colors::Black);  // 不透明黑色 (BGRA: A=255,R=0,G=0,B=0)
    uint32_t bgInfo   = bitmapColor(palette.panelBackground);  // 信息区背景

    for (int dy = 0; dy < totalHeight; dy++) {
        for (int dx = 0; dx < magnifierSize; dx++) {
            if (dy < magnifierSize) {
                // 放大区
                int srcY = magY + dy * captureSizePx / magnifierSize;
                if (srcY < 0 || srcY >= screen.pixelHeight) {
                    impl->bitmapBits[dy * magnifierSize + dx] = bgBlack;
                    continue;
                }
                int srcX = magX + dx * captureSizePx / magnifierSize;
                if (srcX < 0 || srcX >= screen.pixelWidth) {
                    impl->bitmapBits[dy * magnifierSize + dx] = bgBlack;
                    continue;
                }
                const uint8_t* srcRow = screenData + srcY * screen.rowPitch;
                int srcIdx = srcX * 4;
                uint8_t r = srcRow[srcIdx + 0];  // RGBA source
                uint8_t g = srcRow[srcIdx + 1];
                uint8_t b = srcRow[srcIdx + 2];
                // BGRA for DIB (alpha=255, fully opaque)
                impl->bitmapBits[dy * magnifierSize + dx] =
                    (255u << 24) | (r << 16) | (g << 8) | b;
            } else {
                // 信息区背景
                impl->bitmapBits[dy * magnifierSize + dx] = bgInfo;
            }
        }
    }

    // ── 更新像素 RGB 数据（与 Mac 版一致） ──
    if (localX >= 0 && localX < screen.pixelWidth &&
        localY >= 0 && localY < screen.pixelHeight) {
        const uint8_t* pixel = screenData + localY * screen.rowPitch + localX * 4;
        overlay->m_pixelR = pixel[0];
        overlay->m_pixelG = pixel[1];
        overlay->m_pixelB = pixel[2];
    }

    // 逻辑坐标（Windows V2 需将物理像素转为逻辑坐标）
    overlay->m_pixelX = static_cast<int>(localX / sf);
    overlay->m_pixelY = static_cast<int>(localY / sf);

    // ── 十字线（只在放大区内） ──
    int centerX = magnifierSize / 2;
    int centerY = magnifierSize / 2;
    // 与 Mac 版共用十字线颜色。
    uint32_t crossColor = Colors::ToBGRA(Colors::MagnifierCrosshair);
    DrawCrosshair(impl->bitmapBits, magnifierSize, magnifierSize,
                  centerX, centerY, crossThickness, crossColor);

    // ── 分隔线（放大区底部，信息区顶部） ──
    uint32_t separatorColor = bitmapColor(palette.border);
    for (int dx = ScaleInt(4, uiScale); dx < magnifierSize - ScaleInt(4, uiScale); dx++) {
        impl->bitmapBits[magnifierSize * magnifierSize + dx] = separatorColor;
    }

    // ── 信息区文字 ──
    int infoTop = magnifierSize;

    // 第一行：坐标
    wchar_t coordText[64];
    FormatMagnifierText(coordText, 64, "%s  %d,%d", Tr(overlay->m_language, TextId::MagnifierPosition),
               overlay->m_pixelX, overlay->m_pixelY);
    DrawTextLine(impl->hdcMem, impl->hFont, impl->bitmapBits,
                 magnifierSize, totalHeight,
                 coordText, textX, infoTop + ScaleInt(8, uiScale), lineHeight,
                 nativeColor(palette.text), nativeColor(palette.panelBackground));

    wchar_t rgbText[64];
    FormatMagnifierText(rgbText, 64, "%s  #%02X%02X%02X", Tr(overlay->m_language, TextId::MagnifierColor),
               overlay->m_pixelR, overlay->m_pixelG, overlay->m_pixelB);
    DrawTextLine(impl->hdcMem, impl->hFont, impl->bitmapBits,
                 magnifierSize, totalHeight,
                 rgbText, textX, infoTop + ScaleInt(28, uiScale), lineHeight,
                 nativeColor(palette.text), nativeColor(palette.panelBackground));

    wchar_t shortcutText[64];
    FormatMagnifierText(shortcutText, 64, "%s", Tr(overlay->m_language, TextId::MagnifierCopyWin));
    DrawTextLine(impl->hdcMem, impl->hSmallFont, impl->bitmapBits,
                 magnifierSize, totalHeight,
                 shortcutText, textX, infoTop + ScaleInt(48, uiScale), lineHeight,
                 nativeColor(palette.secondaryText), nativeColor(palette.panelBackground));

    ApplyRoundedCornersAndBorder(impl->bitmapBits, magnifierSize, totalHeight, cornerRadius);

    // ── 定位窗口 ──
    POINT ptDst;
    ptDst.x = mouseX + magOffset;
    ptDst.y = mouseY + magOffset;

    // 边界修正：保持在虚拟屏幕内
    int screenRight = GetSystemMetrics(SM_XVIRTUALSCREEN) +
                      GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screenBottom = GetSystemMetrics(SM_YVIRTUALSCREEN) +
                       GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (ptDst.x + magnifierSize > screenRight)
        ptDst.x = mouseX - magnifierSize - magOffset;
    if (ptDst.y + totalHeight > screenBottom)
        ptDst.y = mouseY - totalHeight - magOffset;

    // UpdateLayeredWindow
    POINT ptSrc = {0, 0};
    SIZE sizeWnd = {magnifierSize, totalHeight};
    BLENDFUNCTION blend = {};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(impl->hwnd, nullptr, &ptDst, &sizeWnd, impl->hdcMem,
                        &ptSrc, 0, &blend, ULW_ALPHA);

    // 保持放大镜窗口在 overlay 窗口之上
    SetWindowPos(impl->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

static LRESULT CALLBACK MagnifierWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                         LPARAM lParam) {
    switch (msg) {
    case WM_DESTROY:
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ─── MagnifierOverlay implementation ───

shotlet::MagnifierOverlay::MagnifierOverlay()
    : m_impl(std::make_unique<Impl>()) {}

shotlet::MagnifierOverlay::~MagnifierOverlay() { Shutdown(); }

bool shotlet::MagnifierOverlay::Initialize() {
    if (m_impl->hwnd) return true;
    const float uiScale = ClampUIScale(GetUIScale());
    const int magnifierSize = ScaleInt(kMagnifierSize, uiScale);
    const int infoHeight = ScaleInt(kInfoHeight, uiScale);
    const int totalHeight = magnifierSize + infoHeight;

    // Register window class (once)
    if (!s_classRegistered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.lpfnWndProc = MagnifierWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = kMagnifierClassName;
        wc.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));

        if (!RegisterClassExW(&wc)) {
            Logger::Error("Failed to register magnifier window class");
            return false;
        }
        s_classRegistered = true;
    }

    // Create layered window
    m_impl->hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_TOOLWINDOW |
            WS_EX_NOACTIVATE,
        kMagnifierClassName, L"Shotlet Magnifier", WS_POPUP, 0, 0,
        magnifierSize, totalHeight, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);

    if (!m_impl->hwnd) {
        Logger::Error("Failed to create magnifier window");
        return false;
    }

    // Store this pointer
    SetWindowLongPtrW(m_impl->hwnd, GWLP_USERDATA,
                      reinterpret_cast<LONG_PTR>(this));
    m_impl->owner = this;

    // Create DIB section
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = magnifierSize;
    bmi.bmiHeader.biHeight = -totalHeight; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    m_impl->hdcMem = CreateCompatibleDC(nullptr);
    m_impl->hBitmap = CreateDIBSection(
        m_impl->hdcMem, &bmi, DIB_RGB_COLORS,
        reinterpret_cast<void**>(&m_impl->bitmapBits), nullptr, 0);

    if (!m_impl->hBitmap) {
        Logger::Error("Failed to create magnifier DIB section");
        DestroyWindow(m_impl->hwnd);
        m_impl->hwnd = nullptr;
        return false;
    }

    SelectObject(m_impl->hdcMem, m_impl->hBitmap);

    // 创建信息区字体（主文字 12pt，快捷键提示 10pt）
    m_impl->hFont = CreateFontW(-ScaleInt(12, uiScale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                 CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                 DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");
    m_impl->hSmallFont = CreateFontW(-ScaleInt(10, uiScale), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                      CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                      DEFAULT_PITCH | FF_DONTCARE, L"Microsoft YaHei");

    Logger::Info("Magnifier overlay initialized (Win32 layered window, {}x{})",
                 magnifierSize, totalHeight);
    return true;
}

void shotlet::MagnifierOverlay::Shutdown() {
    if (m_impl->hFont) { DeleteObject(m_impl->hFont); m_impl->hFont = nullptr; }
    if (m_impl->hSmallFont) { DeleteObject(m_impl->hSmallFont); m_impl->hSmallFont = nullptr; }
    if (m_impl->hBitmap) {
        DeleteObject(m_impl->hBitmap);
        m_impl->hBitmap = nullptr;
        m_impl->bitmapBits = nullptr;
    }
    if (m_impl->hdcMem) {
        DeleteDC(m_impl->hdcMem);
        m_impl->hdcMem = nullptr;
    }
    if (m_impl->hwnd) {
        SetWindowLongPtrW(m_impl->hwnd, GWLP_USERDATA, 0);
        DestroyWindow(m_impl->hwnd);
        m_impl->hwnd = nullptr;
    }
    m_visible = false;
}

void shotlet::MagnifierOverlay::Show() {
    if (!m_impl->hwnd) {
        if (!Initialize()) return;
    }

    // Render a frame before showing to avoid flicker
    RefreshMousePosition();
    RenderMagnifier(this);

    ShowWindow(m_impl->hwnd, SW_SHOWNOACTIVATE);
    // 确保放大镜窗口在 overlay 窗口之上
    SetWindowPos(m_impl->hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    m_visible = true;
}

void shotlet::MagnifierOverlay::Hide() {
    if (m_impl->hwnd) {
        ShowWindow(m_impl->hwnd, SW_HIDE);
    }
    m_visible = false;
}

void shotlet::MagnifierOverlay::RefreshMousePosition() {
    POINT pt;
    GetCursorPos(&pt);
    UpdateMousePosition(pt.x, pt.y);
}

void shotlet::MagnifierOverlay::UpdateMousePosition(int x, int y) {
    m_mouseX = x;
    m_mouseY = y;

    if (m_visible && m_impl->hwnd) {
        RenderMagnifier(this);
    }
}

void shotlet::MagnifierOverlay::SetScreenData(std::vector<ScreenData>&& screens) {
    m_screens = std::move(screens);

    if (m_visible && m_impl->hwnd) {
        RenderMagnifier(this);
    }
}

void shotlet::MagnifierOverlay::ClearScreenData() {
    m_screens.clear();
}

void shotlet::MagnifierOverlay::SetScreenshotData(int width, int height,
                                                 const uint8_t* data,
                                                 int rowPitch, int x, int y,
                                                 float scaleFactor) {
    // Compatibility wrapper: package as single-screen data
    ScreenData sd;
    sd.pixelWidth = width;
    sd.pixelHeight = height;
    sd.screenX = x;
    sd.screenY = y;
    sd.rowPitch = rowPitch;
    sd.scaleFactor = scaleFactor;
    sd.displayId = -1;
    int dataSize = rowPitch * height;
    sd.data = std::make_unique<uint8_t[]>(dataSize);
    memcpy(sd.data.get(), data, dataSize);

    m_screens.clear();
    m_screens.push_back(std::move(sd));

    if (m_visible && m_impl->hwnd) {
        RenderMagnifier(this);
    }
}

void shotlet::MagnifierOverlay::UpdateScreenshot() {
    if (m_visible && m_impl->hwnd) {
        RenderMagnifier(this);
    }
}

const shotlet::ScreenData* shotlet::MagnifierOverlay::GetScreenData(int displayId) const {
    for (const auto& sd : m_screens) {
        if (sd.displayId == displayId) return &sd;
    }
    return nullptr;
}

void shotlet::MagnifierOverlay::CopyColorToClipboard() {
    // 格式化色值为 "#RRGGBB"
    char colorStr[8];
    snprintf(colorStr, sizeof(colorStr), "#%02X%02X%02X",
             m_pixelR, m_pixelG, m_pixelB);

    // 转换为宽字符
    wchar_t wColorStr[8];
    MultiByteToWideChar(CP_UTF8, 0, colorStr, -1, wColorStr, 8);

    if (OpenClipboard(m_impl ? m_impl->hwnd : nullptr)) {
        EmptyClipboard();
        size_t len = wcslen(wColorStr) + 1;
        HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, len * sizeof(wchar_t));
        if (hMem) {
            wchar_t* pMem = static_cast<wchar_t*>(GlobalLock(hMem));
            if (pMem) {
                memcpy(pMem, wColorStr, len * sizeof(wchar_t));
                GlobalUnlock(hMem);
                SetClipboardData(CF_UNICODETEXT, hMem);
            }
        }
        CloseClipboard();
        Logger::Info("[MagnifierOverlay] 复制色值到剪贴板: {}", colorStr);
    }
}
