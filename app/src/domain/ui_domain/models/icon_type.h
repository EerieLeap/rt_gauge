#pragma once

#include <cstdint>
#include <lvgl.h>

namespace eerie_leap::domain::ui_domain::models {

// Append only: ICON_TYPE is persisted as an integer.
enum class IconType : std::uint32_t {
    None = 0,
    Label,
    Image,
    Svg, // TODO: Implement SVG Icon
    Rectangle,
    TriangleIsosceles,
    TriangleRight,
    Oval,
    Line
};

} // namespace eerie_leap::domain::ui_domain::models
