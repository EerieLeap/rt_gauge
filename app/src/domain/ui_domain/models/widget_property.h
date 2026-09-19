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
    OPACITY,                // int (0-255)
    COLOR_PRIMARY_ACTIVE,       // string in format: "#RRGGBBAA" eg "#FF0000FF"
    COLOR_PRIMARY_INACTIVE,     // string in format: "#RRGGBBAA" eg "#FF0000FF"
    COLOR_SECONDARY_ACTIVE,     // string in format: "#RRGGBBAA" eg "#FF0000FF"
    COLOR_SECONDARY_INACTIVE,   // string in format: "#RRGGBBAA" eg "#FF0000FF"
    COLOR_TERTIARY_ACTIVE,      // string in format: "#RRGGBBAA" eg "#FF0000FF"
    COLOR_TERTIARY_INACTIVE,    // string in format: "#RRGGBBAA" eg "#FF0000FF"
    ANIMATION_TYPE = 41,        // int (Animation::Type)
    IS_ANIMATION_ACTIVE = 42,   // bool (requested enablement)
    ANIMATION_DURATION_MS = 43, // int (milliseconds per complete cycle)
    COUNT                  // Sentinel, not a property
};

constexpr bool IsValidWidgetPropertyType(WidgetPropertyType type) {
    return type > WidgetPropertyType::NONE && type < WidgetPropertyType::COUNT;
}

} // namespace eerie_leap::domain::ui_domain::models
