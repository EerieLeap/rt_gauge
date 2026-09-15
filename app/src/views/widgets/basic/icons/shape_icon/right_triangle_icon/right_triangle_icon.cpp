#include "right_triangle_icon.h"

namespace eerie_leap::views::widgets::basic::icons {

PolygonIconBase::Polygon RightTriangleIcon::GetVertices(float width, float height) const {
    return { { 0, 0 }, { width, height }, { 0, height } };
}

} // namespace eerie_leap::views::widgets::basic::icons
