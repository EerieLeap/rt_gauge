#pragma once

#include "domain/ui_domain/models/widget_direction.h"
#include "views/widgets/basic/icons/shape_icon/shape_icon_base.h"

namespace eerie_leap::views::widgets::basic::icons {

using eerie_leap::domain::ui_domain::models::WidgetDirection;

class LineIcon : public ShapeIconBase {
private:
    WidgetDirection direction_ = WidgetDirection::LeftToRight;

    bool IsHorizontal() const;
    bool ReadGeometry() override;
    lv_point_t GetBounds() const override;
    bool IsDrawable() const override;
    float BuildPath(lv_vector_path_t* path, float width, float height) const override;

public:
    using ShapeIconBase::ShapeIconBase;
    static void RegisterProperties(WidgetPropertyStore& store);
    [[nodiscard]] IconType GetIconType() const override { return IconType::Line; }
};

} // namespace eerie_leap::views::widgets::basic::icons
