#include "rectangle_icon.h"

namespace eerie_leap::views::widgets::basic::icons {

PolygonIconBase::Polygon RectangleIcon::GetVertices(float width, float height) const {
    return { { 0, 0 }, { width, 0 }, { width, height }, { 0, height } };
}

} // namespace eerie_leap::views::widgets::basic::icons
