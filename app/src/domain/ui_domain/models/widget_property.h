#pragma once

#include <cstdint>
#include <array>
#include <stdexcept>
#include <string_view>

namespace eerie_leap::domain::ui_domain::models {

using namespace std::string_view_literals;

// Persisted as the widget property key - append only, and keep
// WidgetPropertyTypeNames below in the exact same order.
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
};

class WidgetProperty {
private:
    static constexpr const std::array WidgetPropertyTypeNames = {
        "NONE"sv,
        "IS_ACTIVE"sv,
        "IS_SMOOTHED"sv,
        "MIN_VALUE"sv,
        "MAX_VALUE"sv,
        "CHART_POINT_COUNT"sv,
        "CHART_TYPE"sv,
        "LABEL"sv,
        "VALUE_PRECISION"sv,
        "EDGE_OFFSET"sv,
        "POSITION_X"sv,
        "POSITION_Y"sv,
        "POSITION_ANGLE"sv,
        "ICON_TYPE"sv,
        "START_ANGLE"sv,
        "END_ANGLE"sv,
        "FILE_PATH"sv,
        "IMG_WIDTH"sv,
        "IMG_HEIGHT"sv,
        "PIVOT_X"sv,
        "PIVOT_Y"sv,
        "DIRECTION"sv,
        "SETTING_ID"sv,
        "STEP"sv,
        "UNIT"sv,
        "TARGET_SCREEN_GROUP"sv,
        "IS_VISIBLE"sv,
        "VALUE"sv,
        "NAVIGATION_INTENT"sv,
        "WIDTH_PX"sv,
        "HEIGHT_PX"sv,
        "STROKE_PX"sv,
        "CORNER_RAD_PX"sv,
        "FILL_MODE"sv,
    };

public:
    static const char* GetTypeName(WidgetPropertyType type) {
        int index = static_cast<int>(type);
        if(index < 0 || static_cast<size_t>(index) >= WidgetPropertyTypeNames.size())
            throw std::runtime_error("Invalid WidgetPropertyType name conversion.");

        return WidgetPropertyTypeNames[index].data();
    }

    static WidgetPropertyType GetType(std::string_view name) {
        for(size_t i = 0; i < size(WidgetPropertyTypeNames); ++i)
            if(WidgetPropertyTypeNames[i] == name)
                return static_cast<WidgetPropertyType>(i);

        throw std::runtime_error("Invalid widget property type.");
    }
};

} // namespace eerie_leap::domain::ui_domain::models
