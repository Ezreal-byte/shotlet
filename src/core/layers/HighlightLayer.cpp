#include "../Layer.h"

namespace shotlet {
namespace {
constexpr float SelectionStrokeWidth = 1;
}
HighlightLayer::HighlightLayer(ImVec2 start, ImVec2 end, bool ellipse)
    : RectangleLayer(start, end, Colors::Black, SelectionStrokeWidth), m_ellipse(ellipse) {
    SetShowBorder(false);
}
bool HighlightLayer::IsPointNearBorder(const ImVec2& point, float) const {
    const auto bounds = GetBounds();
    if (!m_ellipse) return bounds.Contains(point);
    if (bounds.GetWidth() <= 0 || bounds.GetHeight() <= 0) return false;
    const auto center = bounds.GetCenter();
    const float x = (point.x - center.x) / (bounds.GetWidth() / 2);
    const float y = (point.y - center.y) / (bounds.GetHeight() / 2);
    return x * x + y * y <= 1;
}
std::unique_ptr<Layer> HighlightLayer::Clone() const {
    return std::make_unique<HighlightLayer>(GetStart(), GetEnd(), m_ellipse);
}
}
