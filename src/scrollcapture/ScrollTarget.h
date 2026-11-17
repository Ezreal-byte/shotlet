#pragma once
#include "scrollcapture/ScrollCaptureTypes.h"
#include <vector>

namespace shotlet {
struct ScrollTargetWindow { uint32_t id = 0; int pid = 0; GlobalRect bounds; };
inline bool Contains(const GlobalRect& r, int x, int y) {
    return x >= r.x && y >= r.y && int64_t(x) < int64_t(r.x) + r.w && int64_t(y) < int64_t(r.y) + r.h;
}
inline bool Intersects(const GlobalRect& a, const GlobalRect& b) {
    return int64_t(a.x) < int64_t(b.x) + b.w && int64_t(b.x) < int64_t(a.x) + a.w &&
           int64_t(a.y) < int64_t(b.y) + b.h && int64_t(b.y) < int64_t(a.y) + a.h;
}
// windows 为从前到后的可见窗口，调用方已排除自身窗口与桌面。
inline bool ValidateScrollTarget(const std::vector<ScrollTargetWindow>& windows,
                                  ScrollTargetWindow& locked, const GlobalRect& selection,
                                  int x, int y) {
    if (!locked.id) {
        for (const auto& w : windows) {
            if (Contains(w.bounds, x, y)) { locked = w; break; }
        }
        if (!locked.id) return false;
    }
    for (const auto& w : windows) {
        if (w.id == locked.id) {
            const auto& a = w.bounds; const auto& b = locked.bounds;
            return w.pid == locked.pid && a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
        }
        if (Intersects(w.bounds, selection)) return false;
    }
    return false;
}
} // namespace shotlet
