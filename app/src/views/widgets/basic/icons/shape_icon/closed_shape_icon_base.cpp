#include "closed_shape_icon_base.h"

namespace eerie_leap::views::widgets::basic::icons {

void ClosedShapeIconBase::RegisterProperties(WidgetPropertyStore& store) {
    ShapeIconBase::RegisterProperties(store);
    store.Register(WidgetPropertyType::FILL_MODE,
        ConfigValue { static_cast<int>(WidgetFillMode::Filled) }, PropertyChangeEffect::Repaint);
}

bool ClosedShapeIconBase::ReadGeometry() {
    bool changed = ShapeIconBase::ReadGeometry();
    auto fill = properties_->GetAs<double>(WidgetPropertyType::FILL_MODE, 0) == static_cast<int>(WidgetFillMode::Outline)
        ? WidgetFillMode::Outline : WidgetFillMode::Filled;
    changed |= UpdateValue(fill_mode_, fill);
    return changed;
}

bool ClosedShapeIconBase::IsDrawable() const {
    return fill_mode_ == WidgetFillMode::Filled || stroke_px_ > 0;
}

} // namespace eerie_leap::views::widgets::basic::icons
