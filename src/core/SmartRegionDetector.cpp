#include "SmartRegionDetector.h"

namespace shotlet {

static bool ImRectContains(const ImRect& r, float x, float y) {
    return x >= r.Min.x && x <= r.Max.x && y >= r.Min.y && y <= r.Max.y;
}

static bool ImRectOverlaps(const ImRect& a, const ImRect& b) {
    return a.Min.x < b.Max.x && a.Max.x > b.Min.x &&
           a.Min.y < b.Max.y && a.Max.y > b.Min.y;
}

static bool IsPointOccluded(float x, float y, const std::vector<ImRect>& occluders) {
    for (const auto& o : occluders) {
        if (ImRectContains(o, x, y)) return true;
    }
    return false;
}

#if defined(SHOTLET_PLATFORM_WINDOWS)
static bool IsShellTrayClass(const std::string& className) {
    return className == "Shell_TrayWnd" ||
           className == "Shell_SecondaryTrayWnd";
}
#endif

void SmartRegionDetector::Clear() {
    m_cache.clear();
    m_screenRegion = SmartRegion{};
}

void SmartRegionDetector::BuildCacheFromWindows(const std::vector<RawWindow>& windows,
                                                float screenOffsetX, float screenOffsetY,
                                                float screenW, float screenH) {
    m_cache.clear();
    m_screenRegion = SmartRegion{};

    if (screenW > 0.0f && screenH > 0.0f) {
        m_screenRegion.bounds = ImRect(ImVec2(0.0f, 0.0f), ImVec2(screenW, screenH));
        m_screenRegion.label = "Screen";
    }

    for (size_t i = 0; i < windows.size(); i++) {
        CachedWindow cw;
        cw.globalBounds = windows[i].globalBounds;
        cw.screenOffsetX = screenOffsetX;
        cw.screenOffsetY = screenOffsetY;

        bool collectZOrderOccluders = true;
#if defined(SHOTLET_PLATFORM_WINDOWS)
        collectZOrderOccluders = !IsShellTrayClass(windows[i].className);
#endif
        if (collectZOrderOccluders) {
            cw.occluders.reserve(i);
            for (size_t j = 0; j < i; j++) {
                if (ImRectOverlaps(windows[j].globalBounds, windows[i].globalBounds)) {
                    cw.occluders.push_back(windows[j].globalBounds);
                }
            }
        }

#if defined(SHOTLET_PLATFORM_WINDOWS)
        // 任务栏可能在 EnumWindows 结果中排在普通窗口之后，但视觉上覆盖屏幕底部。
        // 将任务栏补充为遮挡区域，避免过期或偏大的窗口边界在任务栏区域胜出。
        if (!IsShellTrayClass(windows[i].className)) {
            for (size_t j = i + 1; j < windows.size(); j++) {
                if (IsShellTrayClass(windows[j].className) &&
                    ImRectOverlaps(windows[j].globalBounds, windows[i].globalBounds)) {
                    cw.occluders.push_back(windows[j].globalBounds);
                }
            }
        }
#endif

        float lx = windows[i].globalBounds.Min.x - screenOffsetX;
        float ly = windows[i].globalBounds.Min.y - screenOffsetY;
        float rx = windows[i].globalBounds.Max.x - screenOffsetX;
        float ry = windows[i].globalBounds.Max.y - screenOffsetY;

        lx = ImMax(lx, 0.0f);
        ly = ImMax(ly, 0.0f);
        rx = ImMin(rx, screenW);
        ry = ImMin(ry, screenH);

        if (rx <= lx || ry <= ly) {
            continue;
        }

        cw.localRegion.bounds = ImRect(ImVec2(lx, ly), ImVec2(rx, ry));
        cw.localRegion.label = windows[i].label;

        m_cache.push_back(std::move(cw));
    }
}

const SmartRegion* SmartRegionDetector::FindBestRegion(float localMouseX, float localMouseY) const {
    for (const auto& cw : m_cache) {
        if (!ImRectContains(cw.localRegion.bounds, localMouseX, localMouseY))
            continue;

        float globalX = localMouseX + cw.screenOffsetX;
        float globalY = localMouseY + cw.screenOffsetY;

        if (!IsPointOccluded(globalX, globalY, cw.occluders)) {
            return &cw.localRegion;
        }
    }

    if (m_screenRegion.bounds.GetWidth() > 0.0f &&
        m_screenRegion.bounds.GetHeight() > 0.0f &&
        m_screenRegion.bounds.Contains(ImVec2(localMouseX, localMouseY))) {
        return &m_screenRegion;
    }

    return nullptr;
}

} // namespace shotlet
