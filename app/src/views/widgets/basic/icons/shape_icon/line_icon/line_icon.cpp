#include <algorithm>
#include <cmath>

#include "line_icon.h"

namespace eerie_leap::views::widgets::basic::icons {

void LineIcon::RegisterProperties(WidgetPropertyStore& store) {
    ShapeIconBase::RegisterProperties(store);
    store.Register(WidgetPropertyType::DIRECTION,
        ConfigValue { static_cast<int>(WidgetDirection::LeftToRight) }, PropertyChangeEffect::Relayout);
}

bool LineIcon::ReadGeometry() {
    bool changed = ShapeIconBase::ReadGeometry();
    double value = properties_->GetAs<double>(WidgetPropertyType::DIRECTION, 1);
    auto direction = value >= 1 && value <= 4 && std::floor(value) == value
        ? static_cast<WidgetDirection>(static_cast<int>(value)) : WidgetDirection::LeftToRight;
    changed |= UpdateValue(direction_, direction);
    return changed;
}

bool LineIcon::IsHorizontal() const {
    return direction_ == WidgetDirection::LeftToRight || direction_ == WidgetDirection::RightToLeft;
}

lv_point_t LineIcon::GetBounds() const {
    auto bounds = ShapeIconBase::GetBounds();
    if(bounds.x >= 0 && bounds.y >= 0) {
        if(IsHorizontal())
            bounds.y = std::max(bounds.y, stroke_px_);
        else
            bounds.x = std::max(bounds.x, stroke_px_);
    }
    return bounds;
}

bool LineIcon::IsDrawable() const {
    return stroke_px_ > 0;
}

float LineIcon::BuildPath(lv_vector_path_t* path, float width, float height) const {
    bool horizontal = IsHorizontal();
    lv_fpoint_t start { horizontal ? 0.0F : width / 2, horizontal ? height / 2 : 0.0F };
    lv_fpoint_t end { horizontal ? width : width / 2, horizontal ? height / 2 : height };
    if(direction_ == WidgetDirection::RightToLeft || direction_ == WidgetDirection::BottomToTop)
        std::swap(start, end);
    lv_vector_path_move_to(path, &start);
    lv_vector_path_line_to(path, &end);
    return stroke_px_;
}

} // namespace eerie_leap::views::widgets::basic::icons
