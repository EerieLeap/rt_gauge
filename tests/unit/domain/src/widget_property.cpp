#include <array>

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

// Persisted numeric IDs must stay stable when properties are added.
ZTEST(widget_property, test_property_ids_preserve_persisted_values) {
    for(size_t i = 0; i < all_types.size(); ++i)
        zassert_equal(static_cast<uint16_t>(all_types[i]), i);
}

ZTEST(widget_property, test_every_property_type_is_valid) {
    for(auto type : all_types)
        zassert_equal(IsValidWidgetPropertyType(type), type != WidgetPropertyType::NONE);
}

ZTEST(widget_property, test_sentinels_and_unknown_types_are_invalid) {
    zassert_false(IsValidWidgetPropertyType(WidgetPropertyType::NONE));
    zassert_false(IsValidWidgetPropertyType(WidgetPropertyType::COUNT));
    zassert_false(IsValidWidgetPropertyType(static_cast<WidgetPropertyType>(9999)));
    zassert_false(IsValidWidgetPropertyType(static_cast<WidgetPropertyType>(UINT16_MAX)));
}
