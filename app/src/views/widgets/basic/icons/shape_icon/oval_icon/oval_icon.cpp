#include <algorithm>

#include "oval_icon.h"

namespace eerie_leap::views::widgets::basic::icons {

float OvalIcon::BuildPath(lv_vector_path_t* path, float width, float height) const {
    const lv_fpoint_t center{ width / 2, height / 2 };
    const auto stroke_px = static_cast<float>(stroke_px_);
    const float stroke = fill_mode_ == WidgetFillMode::Outline && stroke_px * 2 < std::min(width, height) ? stroke_px
                                                                                                          : 0.0F;
    lv_vector_path_append_circle(path, &center, (width - stroke) / 2, (height - stroke) / 2);
    return stroke;
}

} // namespace eerie_leap::views::widgets::basic::icons
