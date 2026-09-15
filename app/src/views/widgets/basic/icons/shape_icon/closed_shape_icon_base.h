#pragma once

#include "domain/ui_domain/models/widget_fill_mode.h"
#include "shape_icon_base.h"

namespace eerie_leap::views::widgets::basic::icons {

using eerie_leap::domain::ui_domain::models::WidgetFillMode;

class ClosedShapeIconBase : public ShapeIconBase {
protected:
    WidgetFillMode fill_mode_ = WidgetFillMode::Filled;

    bool ReadGeometry() override;
    bool IsDrawable() const override;

public:
    using ShapeIconBase::ShapeIconBase;
    static void RegisterProperties(WidgetPropertyStore& store);
};

} // namespace eerie_leap::views::widgets::basic::icons
