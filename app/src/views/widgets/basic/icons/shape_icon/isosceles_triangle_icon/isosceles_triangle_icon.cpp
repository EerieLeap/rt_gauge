#include "isosceles_triangle_icon.h"

namespace eerie_leap::views::widgets::basic::icons {

PolygonIconBase::Polygon IsoscelesTriangleIcon::GetVertices(float width, float height) const {
    return { { width / 2, 0 }, { width, height }, { 0, height } };
}

} // namespace eerie_leap::views::widgets::basic::icons
