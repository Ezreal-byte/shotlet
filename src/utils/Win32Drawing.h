#pragma once
#include "platform/PlatformWin32.h"
#include "Colors.h"
#include "ScopeExit.h"
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <string_view>

namespace shotlet::Win32Drawing {
inline Gdiplus::Color Color(ImU32 value) { return Gdiplus::Color(Colors::ToBGRA(value)); }

inline void FillBackground(HDC dc, RECT bounds, ImU32 color) {
    const int saved = SaveDC(dc);
    if (!saved) return;
    const ScopeExit restore([&] { RestoreDC(dc, saved); });
    SetDCBrushColor(dc, Colors::ToNativeRgb(color));
    FillRect(dc, &bounds, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

inline void RoundedRect(Gdiplus::Graphics& graphics, Gdiplus::RectF bounds, float radius, ImU32 fill,
                        ImU32 border = Colors::Transparent, float strokePixels = 1) {
    if (bounds.Width <= 0 || bounds.Height <= 0) return;
    radius = std::clamp(radius, 0.0f, std::min(bounds.Width, bounds.Height) / 2);
    Gdiplus::GraphicsPath path;
    if (radius <= 0) path.AddRectangle(bounds);
    else {
        const float diameter = radius * 2;
        path.AddArc(bounds.X, bounds.Y, diameter, diameter, 180, 90);
        path.AddArc(bounds.GetRight() - diameter, bounds.Y, diameter, diameter, 270, 90);
        path.AddArc(bounds.GetRight() - diameter, bounds.GetBottom() - diameter, diameter, diameter, 0, 90);
        path.AddArc(bounds.X, bounds.GetBottom() - diameter, diameter, diameter, 90, 90);
        path.CloseFigure();
    }
    Gdiplus::SolidBrush brush(Color(fill));
    graphics.FillPath(&brush, &path);
    if (border != Colors::Transparent && strokePixels > 0) {
        Gdiplus::Pen pen(Color(border), strokePixels);
        graphics.DrawPath(&pen, &path);
    }
}

inline void CenteredText(HDC dc, RECT bounds, std::wstring_view text, HFONT font, ImU32 color) {
    const int saved = SaveDC(dc);
    if (!saved) return;
    const ScopeExit restore([&] { RestoreDC(dc, saved); });
    if (font) SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, Colors::ToNativeRgb(color));
    DrawTextW(dc, text.data(), int(text.size()), &bounds, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}
}
