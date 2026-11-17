#include "CaptureLinux.h"
#include "utils/Logger.h"

#if defined(SHOTLET_PLATFORM_LINUX)

#include <cstring>
#include <cstdlib>
#include <X11/Xmd.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xrandr.h>

namespace shotlet {

CaptureLinux::CaptureLinux() = default;

CaptureLinux::~CaptureLinux() {
    if (m_xDisplay) {
        XCloseDisplay(m_xDisplay);
        m_xDisplay = nullptr;
    }
}

bool CaptureLinux::Initialize() {
    // 检测显示服务器类型
    if (IsWayland()) {
        Logger::Info("Wayland detected, will use XDG Desktop Portal");
        m_initialized = true;
        return true;
    }
    
    // 尝试连接 X11
    m_xDisplay = XOpenDisplay(nullptr);
    if (!m_xDisplay) {
        m_lastError = "Failed to connect to X11 display";
        return false;
    }
    
    m_xRootWindow = DefaultRootWindow(m_xDisplay);
    m_initialized = true;
    
    Logger::Info("X11 screen capture initialized");
    return true;
}

bool CaptureLinux::IsWayland() {
    const char* waylandDisplay = getenv("WAYLAND_DISPLAY");
    const char* xdgSessionType = getenv("XDG_SESSION_TYPE");
    
    if (waylandDisplay) {
        return true;
    }
    
    if (xdgSessionType && strcmp(xdgSessionType, "wayland") == 0) {
        return true;
    }
    
    return false;
}

bool CaptureLinux::HasXDGPortal() {
    // 检查是否有 xdg-desktop-portal 服务
    // 简化实现：假设现代桌面环境都有
    return true;
}

std::vector<DisplayInfo> CaptureLinux::GetDisplays() {
    std::vector<DisplayInfo> displays;
    
    if (IsWayland()) {
        // Wayland 下通过 XDG Portal 或其他方式获取显示器信息
        // 暂时返回一个默认显示器
        Logger::Warning("Wayland display enumeration not fully implemented");
        DisplayInfo info;
        info.id = 0;
        info.name = "Wayland Display";
        info.displayX = 0;
        info.displayY = 0;
        info.displayWidth = 1920;
        info.displayHeight = 1080;
        info.scaleFactor = 1.0f;
        info.isPrimary = true;
        info.refreshRate = 60;
        displays.push_back(info);
        return displays;
    }
    
    if (!m_xDisplay) {
        m_lastError = "X11 not initialized";
        return displays;
    }
    
    // 尝试使用 XRandR
    int eventBase, errorBase;
    if (XRRQueryExtension(m_xDisplay, &eventBase, &errorBase)) {
        XRRScreenResources* resources = XRRGetScreenResources(m_xDisplay, m_xRootWindow);
        if (resources) {
            for (int i = 0; i < resources->ncrtc; i++) {
                XRRCrtcInfo* crtcInfo = XRRGetCrtcInfo(m_xDisplay, resources, resources->crtcs[i]);
                if (crtcInfo && crtcInfo->mode != None) {
                    DisplayInfo info;
                    info.id = i;
                    info.name = "Display " + std::to_string(i + 1);
                    info.displayX = crtcInfo->x;
                    info.displayY = crtcInfo->y;
                    info.displayWidth = crtcInfo->width;
                    info.displayHeight = crtcInfo->height;
                    info.scaleFactor = 1.0f; // TODO: 获取实际DPI
                    info.isPrimary = (i == 0);
                    info.refreshRate = 60;

                    displays.push_back(info);

                    Logger::Debug("Display {}: {}x{} @ ({},{})",
                                 i, info.displayWidth, info.displayHeight, info.displayX, info.displayY);
                }
                if (crtcInfo) {
                    XRRFreeCrtcInfo(crtcInfo);
                }
            }
            XRRFreeScreenResources(resources);
        }
    } else {
        // 回退到 Xinerama
        int xineramaMajor, xineramaMinor;
        if (XineramaQueryExtension(m_xDisplay, &xineramaMajor, &xineramaMinor) &&
            XineramaIsActive(m_xDisplay)) {
            
            int numScreens;
            XineramaScreenInfo* screenInfo = XineramaQueryScreens(m_xDisplay, &numScreens);
            
            for (int i = 0; i < numScreens; i++) {
                DisplayInfo info;
                info.id = i;
                info.name = "Xinerama " + std::to_string(i + 1);
                info.displayX = screenInfo[i].x_org;
                info.displayY = screenInfo[i].y_org;
                info.displayWidth = screenInfo[i].width;
                info.displayHeight = screenInfo[i].height;
                info.scaleFactor = 1.0f;
                info.isPrimary = (i == 0);
                info.refreshRate = 60;

                displays.push_back(info);
            }
            
            XFree(screenInfo);
        } else {
            // 单个显示器
            Screen* screen = DefaultScreenOfDisplay(m_xDisplay);
            DisplayInfo info;
            info.id = 0;
            info.name = "Primary Display";
            info.displayX = 0;
            info.displayY = 0;
            info.displayWidth = WidthOfScreen(screen);
            info.displayHeight = HeightOfScreen(screen);
            info.scaleFactor = 1.0f;
            info.isPrimary = true;
            info.refreshRate = 60;
            displays.push_back(info);
        }
    }
    
    return displays;
}

CaptureImage CaptureLinux::Capture(const CaptureOptions& options) {
    if (IsWayland()) {
        return CaptureXDGPortal(options);
    }
    return CaptureX11(options);
}

CaptureImage CaptureLinux::CaptureX11(const CaptureOptions& options) {
    if (!m_xDisplay) {
        m_lastError = "X11 not initialized";
        return CaptureImage{};
    }
    
    int x = options.captureX;
    int y = options.captureY;
    int width = options.captureW;
    int height = options.captureH;
    
    // 如果未指定区域，捕获整个根窗口
    if (width <= 0 || height <= 0) {
        XWindowAttributes attrs;
        XGetWindowAttributes(m_xDisplay, m_xRootWindow, &attrs);
        width = attrs.width;
        height = attrs.height;
    }
    
    // 捕获图像
    XImage* image = XGetImage(m_xDisplay, m_xRootWindow, x, y, width, height, 
                              AllPlanes, ZPixmap);
    if (!image) {
        m_lastError = "XGetImage failed";
        return CaptureImage{};
    }
    
    // 分配目标缓冲区
    int rowPitch = width * 4;
    uint8_t* data = new uint8_t[rowPitch * height];
    
    // 转换像素格式
    // X11 图像格式可能是 24bit 或 32bit，需要处理
    for (int row = 0; row < height; row++) {
        for (int col = 0; col < width; col++) {
            unsigned long pixel = XGetPixel(image, col, row);
            
            uint8_t r, g, b;
            if (image->bits_per_pixel == 32) {
                r = (pixel >> 16) & 0xFF;
                g = (pixel >> 8) & 0xFF;
                b = pixel & 0xFF;
            } else {
                r = (pixel >> 16) & 0xFF;
                g = (pixel >> 8) & 0xFF;
                b = pixel & 0xFF;
            }
            
            int idx = row * rowPitch + col * 4;
            data[idx + 0] = r;
            data[idx + 1] = g;
            data[idx + 2] = b;
            data[idx + 3] = 255;
        }
    }
    
    XDestroyImage(image);
    
    CaptureImage result;
    result.data = data;
    result.width = width;
    result.height = height;
    result.captureX = x;
    result.captureY = y;
    result.rowPitch = rowPitch;
    result.displayId = options.displayId;
    
    Logger::Info("Captured {}x{} image via X11", width, height);
    
    return result;
}

CaptureImage CaptureLinux::CaptureXDGPortal(const CaptureOptions& options) {
    // TODO: 实现 XDG Desktop Portal 截图
    // 这需要 D-Bus 通信，比较复杂
    // 暂时返回空，实际使用时需要完整实现
    
    Logger::Error("XDG Desktop Portal capture not yet implemented");
    m_lastError = "Wayland capture not implemented";
    return CaptureImage{};
}

std::string CaptureLinux::GetLastErrorMessage() const {
    return m_lastError;
}

} // namespace shotlet

#endif // SHOTLET_PLATFORM_LINUX
