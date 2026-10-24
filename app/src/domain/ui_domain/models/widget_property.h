#pragma once

#include <cstdint>

namespace eerie_leap::domain::ui_domain::models {

// Persisted as widget property keys and binding targets. Append new properties before COUNT;
// never reorder or reuse existing values.
enum class WidgetPropertyType : std::uint16_t {
    NONE = 0,
    IS_ACTIVE = 1,                 // bool
    IS_SMOOTHED = 2,               // bool
    MIN_VALUE = 3,                 // float
    MAX_VALUE = 4,                 // float
    CHART_POINT_COUNT = 5,         // int
    CHART_TYPE = 6,                // int (enum)
    LABEL = 7,                     // string
    VALUE_PRECISION = 8,           // int
    EDGE_OFFSET = 9,               // int
    POSITION_X = 10,               // int
    POSITION_Y = 11,               // int
    POSITION_ANGLE = 12,           // float
    ICON_TYPE = 13,                // int (enum)
    START_ANGLE = 14,              // int
    END_ANGLE = 15,                // int
    FILE_PATH = 16,                // string
    IMG_WIDTH = 17,                // int
    IMG_HEIGHT = 18,               // int
    ANCHOR_POINT_X = 19,           // int
    ANCHOR_POINT_Y = 20,           // int
    DIRECTION = 21,                // int (enum)
    SETTING_ID = 22,               // string
    STEP = 23,                     // double
    UNIT = 24,                     // string
    TARGET_SCREEN_GROUP = 25,      // int (a screen group, or a screen for ShowOverlay)
    IS_VISIBLE = 26,               // bool
    VALUE = 27,                    // double
    NAVIGATION_INTENT = 28,        // int (NavigationIntent)
    WIDTH_PX = 29,                 // int
    HEIGHT_PX = 30,                // int
    STROKE_PX = 31,                // int
    CORNER_RAD_PX = 32,            // int
    FILL_MODE = 33,                // int (WidgetFillMode)
    OPACITY = 34,                  // int (0-255)
    COLOR_PRIMARY_ACTIVE = 35,     // string in format: "#RRGGBBAA" eg "#FF0000FF"
    COLOR_PRIMARY_INACTIVE = 36,   // string in format: "#RRGGBBAA" eg "#FF0000FF"
    COLOR_SECONDARY_ACTIVE = 37,   // string in format: "#RRGGBBAA" eg "#FF0000FF"
    COLOR_SECONDARY_INACTIVE = 38, // string in format: "#RRGGBBAA" eg "#FF0000FF"
    COLOR_TERTIARY_ACTIVE = 39,    // string in format: "#RRGGBBAA" eg "#FF0000FF"
    COLOR_TERTIARY_INACTIVE = 40,  // string in format: "#RRGGBBAA" eg "#FF0000FF"
    ANIMATION_TYPE = 41,           // int (Animation::Type)
    IS_ANIMATION_ACTIVE = 42,      // bool (requested enablement)
    ANIMATION_DURATION_MS = 43,    // int (milliseconds per complete cycle)
    CHILD_WIDGET_IDS = 44,
    // A root widget's box, in its screen's grid cells like the record's position and size (y counts up from the
    // bottom). Binding targets only: the record stays the one place the box is written, so a properties map never
    // holds them, and a child fills its owner, so it has none.
    LAYOUT_X = 45,       // int
    LAYOUT_Y = 46,       // int
    LAYOUT_WIDTH = 47,   // int
    LAYOUT_HEIGHT = 48,  // int
    FONT_FILE_PATH = 49, // string, a `fonts` asset name; empty means the theme font
    FONT_SIZE_PX = 50,   // int, the em size in pixels
    COUNT = 51           // Sentinel, not a property
};

constexpr bool IsValidWidgetPropertyType(WidgetPropertyType type) {
    return type > WidgetPropertyType::NONE && type < WidgetPropertyType::COUNT;
}

} // namespace eerie_leap::domain::ui_domain::models
