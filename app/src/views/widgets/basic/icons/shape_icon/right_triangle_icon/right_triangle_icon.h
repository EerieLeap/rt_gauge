#pragma once

#include "views/widgets/basic/icons/shape_icon/polygon_icon_base.h"

namespace eerie_leap::views::widgets::basic::icons {

class RightTriangleIcon : public PolygonIconBase {
private:
    Polygon GetVertices(float width, float height) const override;

public:
    using PolygonIconBase::PolygonIconBase;
    [[nodiscard]] IconType GetIconType() const override { return IconType::TriangleRight; }
};

} // namespace eerie_leap::views::widgets::basic::icons
