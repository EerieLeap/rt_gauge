#pragma once

#include <cstdint>

namespace eerie_leap::domain::ui_domain::models {

// Persisted as widget property keys and binding targets. Append new properties before COUNT;
// never reorder or reuse existing values.
enum class WidgetPropertyType : std::uint16_t {
    NONE = 0,
    IS_ACTIVE,              // bool
    IS_SMOOTHED,            // bool
    MIN_VALUE,              // float
    MAX_VALUE,              // float
    CHART_POINT_COUNT,      // int
    CHART_TYPE,             // int (enum)
    LABEL,                  // string
    VALUE_PRECISION,        // int
    EDGE_OFFSET,            // int
    POSITION_X,             // int
    POSITION_Y,             // int
    POSITION_ANGLE,         // float
    ICON_TYPE,              // int (enum)
    START_ANGLE,            // int
    END_ANGLE,              // int
    FILE_PATH,              // string
    IMG_WIDTH,              // int
    IMG_HEIGHT,             // int
    PIVOT_X,                // int
    PIVOT_Y,                // int
    DIRECTION,              // int (enum)
    SETTING_ID,             // string
    STEP,                   // double
    UNIT,                   // string
    TARGET_SCREEN_GROUP,    // int (a screen group, or a screen for ShowOverlay)
    IS_VISIBLE,             // bool
    VALUE,                  // double
    NAVIGATION_INTENT,      // int (NavigationIntent)
    WIDTH_PX,               // int
    HEIGHT_PX,              // int
    STROKE_PX,              // int
    CORNER_RAD_PX,          // int
    FILL_MODE,              // int (WidgetFillMode)
    COUNT                  // Sentinel, not a property
};

constexpr bool IsValidWidgetPropertyType(WidgetPropertyType type) {
    return type > WidgetPropertyType::NONE && type < WidgetPropertyType::COUNT;
}

} // namespace eerie_leap::domain::ui_domain::models
