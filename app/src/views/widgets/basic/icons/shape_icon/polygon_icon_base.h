#pragma once

#include <vector>

#include "views/widgets/basic/icons/shape_icon/closed_shape_icon_base.h"

namespace eerie_leap::views::widgets::basic::icons {

// Shares rounded corners and inset outlines for convex polygons. At least three
// vertices must follow the perimeter (either winding), without repeated or
// collinear corners, in the local [0,width] x [0,height] coordinate space.
class PolygonIconBase : public ClosedShapeIconBase {
private:
    int corner_radius_px_ = 0;

    bool ReadGeometry() override;
    float BuildPath(lv_vector_path_t* path, float width, float height) const override;

protected:
    using Polygon = std::vector<lv_fpoint_t>;
    virtual Polygon GetVertices(float width, float height) const = 0;

public:
    using ClosedShapeIconBase::ClosedShapeIconBase;
    static void RegisterProperties(WidgetPropertyStore& store);
};

} // namespace eerie_leap::views::widgets::basic::icons
