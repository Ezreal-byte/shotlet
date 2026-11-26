#include "CaptureLinux.h"
#include "core/CaptureDocument.h"
#include "utils/Logger.h"

#if defined(SHOTLET_PLATFORM_LINUX)
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/Xrandr.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>

namespace shotlet {
    namespace {
        bool IsWaylandSession() {
            const char *display = std::getenv("WAYLAND_DISPLAY");
            const char *session = std::getenv("XDG_SESSION_TYPE");
            return (display && *display) || (session && std::strcmp(session, "wayland") == 0);
        }
        unsigned char ColorComponent(unsigned long pixel, unsigned long mask) {
            if (!mask)
                return 0;
            while (!(mask & 1)) {
                mask >>= 1;
                pixel >>= 1;
            }
            return static_cast<unsigned char>((uint64_t(pixel & mask) * 255 + mask / 2) / mask);
        }
    } // namespace

    struct CaptureLinux::Impl {
        Display *display = nullptr;
        Window root = 0;
        ~Impl() {
            if (display)
                XCloseDisplay(display);
        }
    };
    CaptureLinux::CaptureLinux() = default;
    CaptureLinux::~CaptureLinux() = default;

    bool CaptureLinux::Initialize() {
        m_lastError.clear();
        m_impl.reset();
        if (IsWaylandSession()) {
            m_lastError = "Linux capture currently requires an X11 session. Log in using X11 instead of Wayland.";
            return false;
        }
        auto impl = std::make_unique<Impl>();
        impl->display = XOpenDisplay(nullptr);
        if (!impl->display) {
            m_lastError = "Cannot connect to the X11 desktop. Check DISPLAY and the desktop session permissions.";
            return false;
        }
        impl->root = DefaultRootWindow(impl->display);
        m_impl = std::move(impl);
        Logger::Info("X11 screen capture initialized");
        return true;
    }

    std::vector<DisplayInfo> CaptureLinux::GetDisplays() {
        std::vector<DisplayInfo> displays;
        if (!m_impl) {
            m_lastError = "X11 capture is not initialized";
            return displays;
        }
        m_lastError.clear();
        auto *display = m_impl->display;
        const Window root = m_impl->root;
        int major = 1, minor = 2;
        if (XRRQueryVersion(display, &major, &minor) && (major > 1 || (major == 1 && minor >= 2))) {
            const RROutput primary = (major > 1 || minor >= 3) ? XRRGetOutputPrimary(display, root) : 0;
            auto *resources = XRRGetScreenResources(display, root);
            if (resources) {
                std::set<RRCrtc> seen;
                for (int index = 0; index < resources->noutput; ++index) {
                    const RROutput output = resources->outputs[index];
                    auto *info = XRRGetOutputInfo(display, resources, output);
                    if (info && info->connection == RR_Connected && info->crtc && !seen.contains(info->crtc)) {
                        auto *crtc = XRRGetCrtcInfo(display, resources, info->crtc);
                        if (crtc && crtc->mode && crtc->width && crtc->height) {
                            DisplayInfo item{};
                            item.id = static_cast<int>(output);
                            item.name.assign(info->name, info->nameLen);
                            item.displayX = crtc->x;
                            item.displayY = crtc->y;
                            item.displayWidth = crtc->width;
                            item.displayHeight = crtc->height;
                            // X11 window coordinates and root images both use framebuffer pixels.
                            item.scaleFactor = 1.0f;
                            item.isPrimary = output == primary;
                            for (int mode = 0; mode < resources->nmode; ++mode) {
                                const auto &timing = resources->modes[mode];
                                if (timing.id == crtc->mode && timing.hTotal && timing.vTotal) {
                                    double rate = double(timing.dotClock) / timing.hTotal / timing.vTotal;
                                    if (timing.modeFlags & RR_Interlace)
                                        rate *= 2;
                                    if (timing.modeFlags & RR_DoubleScan)
                                        rate /= 2;
                                    item.refreshRate = static_cast<int>(rate + 0.5);
                                    break;
                                }
                            }
                            displays.push_back(std::move(item));
                            seen.insert(info->crtc);
                        }
                        if (crtc)
                            XRRFreeCrtcInfo(crtc);
                    }
                    if (info)
                        XRRFreeOutputInfo(info);
                }
                XRRFreeScreenResources(resources);
            }
        }
        if (displays.empty() && XineramaIsActive(display)) {
            int count = 0;
            auto *screens = XineramaQueryScreens(display, &count);
            if (screens) {
                for (int index = 0; index < count; ++index) {
                    DisplayInfo item{};
                    item.id = screens[index].screen_number;
                    item.name = "Xinerama " + std::to_string(item.id);
                    item.displayX = screens[index].x_org;
                    item.displayY = screens[index].y_org;
                    item.displayWidth = screens[index].width;
                    item.displayHeight = screens[index].height;
                    item.scaleFactor = 1.0f;
                    item.isPrimary = index == 0;
                    displays.push_back(std::move(item));
                }
                XFree(screens);
            }
        }
        if (displays.empty()) {
            XWindowAttributes attributes{};
            if (XGetWindowAttributes(display, root, &attributes) && attributes.width > 0 && attributes.height > 0) {
                DisplayInfo item{};
                item.id = 0;
                item.name = "X11 Desktop";
                item.displayWidth = attributes.width;
                item.displayHeight = attributes.height;
                item.scaleFactor = 1.0f;
                item.isPrimary = true;
                displays.push_back(std::move(item));
            }
        }
        if (!displays.empty() &&
            std::none_of(displays.begin(), displays.end(), [](const auto &d) { return d.isPrimary; }))
            displays.front().isPrimary = true;
        return displays;
    }

    CaptureImage CaptureLinux::Capture(const CaptureOptions &options) {
        if (!m_impl) {
            m_lastError = "X11 capture is not initialized";
            return {};
        }
        m_lastError.clear();
        XWindowAttributes root{};
        if (!XGetWindowAttributes(m_impl->display, m_impl->root, &root)) {
            m_lastError = "Cannot query X11 desktop bounds";
            return {};
        }
        int64_t left = 0, top = 0, right = root.width, bottom = root.height;
        if (options.displayId >= 0) {
            DisplayInfo selected{};
            if (options.hasDisplayInfo && options.displayInfo.id == options.displayId)
                selected = options.displayInfo;
            else {
                const auto displays = GetDisplays();
                const auto found = std::find_if(displays.begin(), displays.end(),
                                                [&](const auto &d) { return d.id == options.displayId; });
                if (found == displays.end()) {
                    m_lastError = "The requested X11 display is no longer available";
                    return {};
                }
                selected = *found;
            }
            left = selected.displayX;
            top = selected.displayY;
            right = left + selected.displayWidth;
            bottom = top + selected.displayHeight;
        }
        int64_t x = left, y = top, endX = right, endY = bottom;
        if (options.captureW || options.captureH) {
            if (options.captureW <= 0 || options.captureH <= 0) {
                m_lastError = "Invalid capture dimensions";
                return {};
            }
            x += options.captureX;
            y += options.captureY;
            endX = x + options.captureW;
            endY = y + options.captureH;
        }
        x = std::max({x, left, int64_t(0)});
        y = std::max({y, top, int64_t(0)});
        endX = std::min({endX, right, int64_t(root.width)});
        endY = std::min({endY, bottom, int64_t(root.height)});
        if (endX <= x || endY <= y || !DocumentPixelBytes(int(endX - x), int(endY - y))) {
            m_lastError = "Capture region is outside the desktop or exceeds the image size limit";
            return {};
        }
        const int width = int(endX - x), height = int(endY - y);
        auto *image = XGetImage(m_impl->display, m_impl->root, int(x), int(y), width, height, AllPlanes, ZPixmap);
        if (!image) {
            m_lastError = "X11 could not capture the desktop image";
            return {};
        }
        const auto destroyImage = [](XImage *value) { XDestroyImage(value); };
        const std::unique_ptr<XImage, decltype(destroyImage)> releaseImage(image, destroyImage);
        CaptureImage result;
        result.data = new uint8_t[size_t(width) * height * 4];
        result.width = width;
        result.height = height;
        result.rowPitch = width * 4;
        result.captureX = int(x);
        result.captureY = int(y);
        result.displayId = options.displayId;
        for (int row = 0; row < height; ++row) {
            for (int column = 0; column < width; ++column) {
                const auto pixel = XGetPixel(image, column, row);
                auto *rgba = result.data + size_t(row) * result.rowPitch + size_t(column) * 4;
                rgba[0] = ColorComponent(pixel, image->red_mask);
                rgba[1] = ColorComponent(pixel, image->green_mask);
                rgba[2] = ColorComponent(pixel, image->blue_mask);
                rgba[3] = 255;
            }
        }
        Logger::Info("Captured {}x{} at ({},{}) via X11", width, height, x, y);
        return result;
    }

    std::string CaptureLinux::GetLastErrorMessage() const { return m_lastError; }
} // namespace shotlet
#endif
