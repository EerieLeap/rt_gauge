#include <array>
#include <stdexcept>
#include <string_view>

#include <zephyr/ztest.h>

#include "domain/ui_domain/models/widget_property.h"
#include "domain/ui_domain/models/widget_direction.h"
#include "domain/ui_domain/models/widget_fill_mode.h"
#include "domain/ui_domain/models/icon_type.h"

using namespace eerie_leap::domain::ui_domain::models;

ZTEST_SUITE(widget_property, NULL, NULL, NULL, NULL, NULL);

namespace {

constexpr std::array all_types = {
    WidgetPropertyType::NONE,
    WidgetPropertyType::IS_ACTIVE,
    WidgetPropertyType::IS_SMOOTHED,
    WidgetPropertyType::MIN_VALUE,
    WidgetPropertyType::MAX_VALUE,
    WidgetPropertyType::CHART_POINT_COUNT,
    WidgetPropertyType::CHART_TYPE,
    WidgetPropertyType::LABEL,
    WidgetPropertyType::VALUE_PRECISION,
    WidgetPropertyType::EDGE_OFFSET,
    WidgetPropertyType::POSITION_X,
    WidgetPropertyType::POSITION_Y,
    WidgetPropertyType::POSITION_ANGLE,
    WidgetPropertyType::ICON_TYPE,
    WidgetPropertyType::START_ANGLE,
    WidgetPropertyType::END_ANGLE,
    WidgetPropertyType::FILE_PATH,
    WidgetPropertyType::IMG_WIDTH,
    WidgetPropertyType::IMG_HEIGHT,
    WidgetPropertyType::PIVOT_X,
    WidgetPropertyType::PIVOT_Y,
    WidgetPropertyType::DIRECTION,
    WidgetPropertyType::SETTING_ID,
    WidgetPropertyType::STEP,
    WidgetPropertyType::UNIT,
    WidgetPropertyType::TARGET_SCREEN_GROUP,
    WidgetPropertyType::IS_VISIBLE,
    WidgetPropertyType::VALUE,
    WidgetPropertyType::NAVIGATION_INTENT,
    WidgetPropertyType::WIDTH_PX,
    WidgetPropertyType::HEIGHT_PX,
    WidgetPropertyType::STROKE_PX,
    WidgetPropertyType::CORNER_RAD_PX,
    WidgetPropertyType::FILL_MODE
};

} // namespace

ZTEST(widget_property, test_shape_additions_preserve_persisted_values) {
    zassert_equal(static_cast<int>(IconType::Image), 3);
    zassert_equal(static_cast<int>(IconType::Svg), 4);
    zassert_equal(static_cast<int>(WidgetPropertyType::NAVIGATION_INTENT), 28);
    zassert_equal(static_cast<int>(WidgetPropertyType::DIRECTION), 21);
    zassert_equal(static_cast<int>(WidgetDirection::None), 0);
    zassert_equal(static_cast<int>(WidgetDirection::LeftToRight), 1);
    zassert_equal(static_cast<int>(WidgetDirection::RightToLeft), 2);
    zassert_equal(static_cast<int>(WidgetDirection::TopToBottom), 3);
    zassert_equal(static_cast<int>(WidgetDirection::BottomToTop), 4);
    zassert_equal(static_cast<int>(WidgetFillMode::Filled), 0);
    zassert_equal(static_cast<int>(WidgetFillMode::Outline), 1);
}

// The enum and the name table are positional; a name added out of order silently
// re-keys every persisted property after it.
ZTEST(widget_property, test_every_type_round_trips_through_its_name) {
    for(auto type : all_types)
        zassert_equal(
            WidgetProperty::GetType(WidgetProperty::GetTypeName(type)),
            type,
            "Property type %u does not round trip.", static_cast<unsigned>(type));
}

ZTEST(widget_property, test_phase_four_names) {
    zassert_equal(std::string_view(WidgetProperty::GetTypeName(WidgetPropertyType::SETTING_ID)), "SETTING_ID");
    zassert_equal(std::string_view(WidgetProperty::GetTypeName(WidgetPropertyType::STEP)), "STEP");
    zassert_equal(std::string_view(WidgetProperty::GetTypeName(WidgetPropertyType::UNIT)), "UNIT");
    zassert_equal(std::string_view(WidgetProperty::GetTypeName(WidgetPropertyType::TARGET_SCREEN_GROUP)), "TARGET_SCREEN_GROUP");
}

ZTEST(widget_property, test_unknown_name_is_rejected) {
    bool threw = false;

    try {
        WidgetProperty::GetType("NOT_A_PROPERTY");
    } catch(const std::runtime_error&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(widget_property, test_out_of_range_type_is_rejected) {
    bool threw = false;

    try {
        WidgetProperty::GetTypeName(static_cast<WidgetPropertyType>(all_types.size()));
    } catch(const std::runtime_error&) {
        threw = true;
    }

    zassert_true(threw);
}
