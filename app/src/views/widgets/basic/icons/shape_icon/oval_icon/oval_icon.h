#pragma once

#include "views/widgets/basic/icons/shape_icon/closed_shape_icon_base.h"

namespace eerie_leap::views::widgets::basic::icons {

class OvalIcon : public ClosedShapeIconBase {
private:
    float BuildPath(lv_vector_path_t* path, float width, float height) const override;

public:
    using ClosedShapeIconBase::ClosedShapeIconBase;
    [[nodiscard]] IconType GetIconType() const override { return IconType::Oval; }
};

} // namespace eerie_leap::views::widgets::basic::icons
