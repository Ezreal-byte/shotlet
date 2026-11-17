#include "CaptureWindows.h"
#include "capture/ScreenCapture.h"
#include "platform/WindowsDpiAwareness.h"
#include "utils/Logger.h"

#if defined(SHOTLET_PLATFORM_WINDOWS)

#include "platform/PlatformWin32.h"
#include <shellscalingapi.h>

#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace shotlet {

// ─── Helper: Find HMONITOR by displayId ───

struct EnumMonitorContext {
    int targetIndex;
    HMONITOR result;
};

static BOOL CALLBACK EnumMonitorByIndexProc(HMONITOR hMonitor, HDC,
                                            LPRECT, LPARAM dwData) {
    auto* ctx = reinterpret_cast<EnumMonitorContext*>(dwData);
    if (ctx->targetIndex == 0) {
        ctx->result = hMonitor;
        return FALSE;
    }
    ctx->targetIndex--;
    return TRUE;
}

static HMONITOR FindMonitorByDisplayId(int displayId) {
    EnumMonitorContext ctx;
    ctx.targetIndex = displayId;
    ctx.result = nullptr;
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorByIndexProc,
                        reinterpret_cast<LPARAM>(&ctx));
    return ctx.result;
}

// ─── CaptureWindows ───

CaptureWindows::CaptureWindows() = default;

CaptureWindows::~CaptureWindows() = default;

bool CaptureWindows::Initialize() {
    EnsureWindowsDpiAwareness();
    m_initialized = true;
    Logger::Info("Windows screen capture initialized");
    return true;
}

std::vector<DisplayInfo> CaptureWindows::GetDisplays() {
    std::vector<DisplayInfo> displays;

    struct EnumCtx {
        std::vector<DisplayInfo>* list;
    };
    EnumCtx ctx;
    ctx.list = &displays;

    auto enumProc = [](HMONITOR hMonitor, HDC, LPRECT lprcMonitor,
                       LPARAM dwData) -> BOOL {
        auto* enumCtx = reinterpret_cast<EnumCtx*>(dwData);
        auto* dispList = enumCtx->list;
        int idx = static_cast<int>(dispList->size());

        MONITORINFOEXA info;
        info.cbSize = sizeof(MONITORINFOEXA);

        if (GetMonitorInfoA(hMonitor, &info)) {
            DisplayInfo display;
            display.id = idx;
            display.name = info.szDevice;
            display.displayX = info.rcMonitor.left;
            display.displayY = info.rcMonitor.top;
            display.displayWidth = info.rcMonitor.right - info.rcMonitor.left;
            display.displayHeight = info.rcMonitor.bottom - info.rcMonitor.top;
            display.isPrimary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;

            UINT dpiX = 96, dpiY = 96;
            typedef HRESULT(WINAPI * GetDpiForMonitorFunc)(
                HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
            HMODULE hShcore = GetModuleHandleA("shcore.dll");
            if (hShcore) {
                auto getDpiForMonitor =
                    (GetDpiForMonitorFunc)GetProcAddress(hShcore, "GetDpiForMonitor");
                if (getDpiForMonitor) {
                    getDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
                }
            }
            display.scaleFactor = static_cast<float>(dpiX) / 96.0f;
            display.refreshRate = 60;

            dispList->push_back(display);
        }

        return TRUE;
    };

    EnumDisplayMonitors(nullptr, nullptr, enumProc,
                        reinterpret_cast<LPARAM>(&ctx));

    return displays;
}

CaptureImage CaptureWindows::Capture(const CaptureOptions& options) {
    if (options.displayId < 0) {
        m_lastError = "displayId required";
        return CaptureImage{};
    }

    HMONITOR targetMonitor = FindMonitorByDisplayId(options.displayId);
    if (!targetMonitor) {
        m_lastError = "Display not found for displayId=" +
                      std::to_string(options.displayId);
        Logger::Error("{}", m_lastError);
        return CaptureImage{};
    }

    return CaptureDisplayHMONITOR(static_cast<void*>(targetMonitor), options.cursor);
}

CaptureImage CaptureWindows::CaptureDisplayHMONITOR(void* hMonitorVoid,
                                                     bool includeCursor) {
    HMONITOR hMonitor = static_cast<HMONITOR>(hMonitorVoid);
    CaptureImage result{};

    MONITORINFOEXA monitorInfo;
    monitorInfo.cbSize = sizeof(MONITORINFOEXA);
    if (!GetMonitorInfoA(hMonitor, &monitorInfo)) {
        m_lastError = "GetMonitorInfoA failed";
        Logger::Error("{}", m_lastError);
        return result;
    }

    int x = monitorInfo.rcMonitor.left;
    int y = monitorInfo.rcMonitor.top;
    int width = monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left;
    int height = monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top;

    if (width <= 0 || height <= 0) {
        m_lastError = "Invalid monitor dimensions";
        Logger::Error("{}", m_lastError);
        return result;
    }

    // In Per-Monitor V2 DPI mode, GetMonitorInfoA returns physical pixel coordinates.
    // scaleFactor > 1 means the screen has scaling, but rcMonitor is already in physical pixels.
    // So the capture dimensions should equal the monitor dimensions, NOT multiplied by scaleFactor.
    int pixelWidth = width;
    int pixelHeight = height;

    // Compute scaleFactor for OverlayManager (to derive logical coordinates from physical pixels)
    UINT dpiX = 96, dpiY = 96;
    typedef HRESULT(WINAPI * GetDpiForMonitorFunc)(
        HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*);
    HMODULE hShcore = GetModuleHandleA("shcore.dll");
    if (hShcore) {
        auto getDpiForMonitor =
            (GetDpiForMonitorFunc)GetProcAddress(hShcore, "GetDpiForMonitor");
        if (getDpiForMonitor) {
            getDpiForMonitor(hMonitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
        }
    }
    float scaleFactor = static_cast<float>(dpiX) / 96.0f;

    HDC hScreenDC = GetDC(nullptr);
    HDC hMemoryDC = CreateCompatibleDC(hScreenDC);

    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = pixelWidth;
    bmi.bmiHeader.biHeight = -pixelHeight;
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    uint8_t* data = nullptr;
    HBITMAP hBitmap = CreateDIBSection(hMemoryDC, &bmi, DIB_RGB_COLORS,
                                       reinterpret_cast<void**>(&data), nullptr, 0);
    if (!hBitmap || !data) {
        m_lastError = "CreateDIBSection failed";
        Logger::Error("{}", m_lastError);
        DeleteDC(hMemoryDC);
        ReleaseDC(nullptr, hScreenDC);
        return result;
    }

    HBITMAP hOldBitmap = (HBITMAP)SelectObject(hMemoryDC, hBitmap);

    SetStretchBltMode(hMemoryDC, STRETCH_HALFTONE);
    StretchBlt(hMemoryDC, 0, 0, pixelWidth, pixelHeight,
               hScreenDC, x, y, width, height, SRCCOPY);

    if (includeCursor) {
        CURSORINFO cursorInfo;
        cursorInfo.cbSize = sizeof(CURSORINFO);
        if (GetCursorInfo(&cursorInfo) && (cursorInfo.flags & CURSOR_SHOWING)) {
            ICONINFO iconInfo;
            if (GetIconInfo(cursorInfo.hCursor, &iconInfo)) {
                int cursorX = static_cast<int>(
                    (cursorInfo.ptScreenPos.x - x) * scaleFactor);
                int cursorY = static_cast<int>(
                    (cursorInfo.ptScreenPos.y - y) * scaleFactor);
                DrawIcon(hMemoryDC, cursorX, cursorY, cursorInfo.hCursor);
                if (iconInfo.hbmMask) DeleteObject(iconInfo.hbmMask);
                if (iconInfo.hbmColor) DeleteObject(iconInfo.hbmColor);
            }
        }
    }

    SelectObject(hMemoryDC, hOldBitmap);
    DeleteDC(hMemoryDC);
    ReleaseDC(nullptr, hScreenDC);

    // CreateDIBSection with biBitCount=32 and biCompression=BI_RGB produces
    // BGRA data (Blue, Green, Red, Alpha/padding). SDL_GPU textures expect RGBA
    // order, so we must swap R↔B and set alpha=255 (GDI leaves alpha at 0).
    int rowPitch = pixelWidth * 4;
    for (int i = 0; i < pixelWidth * pixelHeight; i++) {
        uint8_t temp = data[i * 4 + 0];      // B
        data[i * 4 + 0] = data[i * 4 + 2];  // B → R
        data[i * 4 + 2] = temp;              // R → B
        data[i * 4 + 3] = 255;               // Set alpha to 255
    }

    result.data = data;
    result.gdiHandle = hBitmap;  // 保存 HBITMAP 以便后续正确释放
    result.width = pixelWidth;
    result.height = pixelHeight;
    result.captureX = x;
    result.captureY = y;
    result.rowPitch = rowPitch;
    result.scaleFactor = scaleFactor;

    // Find displayId from HMONITOR
    struct FindIdCtx {
        HMONITOR target;
        int foundId;
        int currentIndex;
    };
    FindIdCtx findCtx;
    findCtx.target = hMonitor;
    findCtx.foundId = 0;
    findCtx.currentIndex = 0;

    auto findIdProc = [](HMONITOR hMon, HDC, LPRECT, LPARAM dwData) -> BOOL {
        auto* ctx2 = reinterpret_cast<FindIdCtx*>(dwData);
        if (hMon == ctx2->target) {
            ctx2->foundId = ctx2->currentIndex;
            return FALSE;
        }
        ctx2->currentIndex++;
        return TRUE;
    };

    EnumDisplayMonitors(nullptr, nullptr, findIdProc,
                        reinterpret_cast<LPARAM>(&findCtx));
    result.displayId = findCtx.foundId;

    Logger::Info("Captured {}x{} @ ({},{}), scale={:.1f}, displayId={}",
                 pixelWidth, pixelHeight, x, y, scaleFactor, result.displayId);

    return result;
}

std::string CaptureWindows::GetLastErrorMessage() const {
    return m_lastError;
}

} // namespace shotlet

#endif // SHOTLET_PLATFORM_WINDOWS
