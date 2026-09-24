#include <array>

#include <zephyr/ztest.h>

#include "domain/ui_domain/models/widget_property.h"
#include "domain/ui_domain/models/animation.h"
#include "domain/ui_domain/models/widget_direction.h"
#include "domain/ui_domain/models/widget_fill_mode.h"
#include "domain/ui_domain/models/icon_type.h"
#include "utilities/type/color.h"

using namespace eerie_leap::domain::ui_domain::models;
using eerie_leap::utilities::type::Color;

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
    WidgetPropertyType::ANCHOR_POINT_X,
    WidgetPropertyType::ANCHOR_POINT_Y,
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
    WidgetPropertyType::FILL_MODE,
    WidgetPropertyType::OPACITY,
    WidgetPropertyType::COLOR_PRIMARY_ACTIVE,
    WidgetPropertyType::COLOR_PRIMARY_INACTIVE,
    WidgetPropertyType::COLOR_SECONDARY_ACTIVE,
    WidgetPropertyType::COLOR_SECONDARY_INACTIVE,
    WidgetPropertyType::COLOR_TERTIARY_ACTIVE,
    WidgetPropertyType::COLOR_TERTIARY_INACTIVE,
    WidgetPropertyType::ANIMATION_TYPE,
    WidgetPropertyType::IS_ANIMATION_ACTIVE,
    WidgetPropertyType::ANIMATION_DURATION_MS,
    WidgetPropertyType::CHILD_WIDGET_IDS
};

} // namespace

ZTEST(widget_property, test_shape_additions_preserve_persisted_values) {
    // Removing the dot icon deliberately renumbered the icon types after Label.
    zassert_equal(static_cast<int>(IconType::Image), 2);
    zassert_equal(static_cast<int>(IconType::Svg), 3);
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
    zassert_equal(all_types.size(), 45);
    zassert_equal(static_cast<uint16_t>(WidgetPropertyType::COUNT), all_types.size());
    zassert_equal(static_cast<uint16_t>(WidgetPropertyType::ANCHOR_POINT_X), 19);
    zassert_equal(static_cast<uint16_t>(WidgetPropertyType::ANCHOR_POINT_Y), 20);
    zassert_equal(static_cast<uint16_t>(WidgetPropertyType::CHILD_WIDGET_IDS), 44);
    for(size_t index = 0; index < all_types.size(); ++index)
        zassert_equal(static_cast<uint16_t>(all_types[index]), index);
}

ZTEST(widget_property, test_every_property_type_is_valid) {
    for(auto type : all_types)
        zassert_equal(IsValidWidgetPropertyType(type), type != WidgetPropertyType::NONE);
}

ZTEST(widget_property, test_animation_ids_defaults_and_duration_contract) {
    zassert_equal(static_cast<int>(Animation::Type::None), 0);
    zassert_equal(static_cast<int>(Animation::Type::Blinking), 1);
    zassert_equal(static_cast<int>(Animation::Type::Rotation), 2);
    zassert_equal(Animation::DEFAULT_TYPE, Animation::Type::None);
    zassert_false(Animation::DEFAULT_ACTIVE);
    zassert_equal(Animation::DEFAULT_DURATION_MS, 1000);
    zassert_equal(Animation::MIN_DURATION_MS, 2);
    zassert_equal(Animation::MAX_DURATION_MS, INT32_MAX);
}

ZTEST(widget_property, test_sentinels_and_unknown_types_are_invalid) {
    zassert_false(IsValidWidgetPropertyType(WidgetPropertyType::NONE));
    zassert_false(IsValidWidgetPropertyType(WidgetPropertyType::COUNT));
    zassert_false(IsValidWidgetPropertyType(static_cast<WidgetPropertyType>(9999)));
    zassert_false(IsValidWidgetPropertyType(static_cast<WidgetPropertyType>(UINT16_MAX)));
}

ZTEST(widget_property, test_color_parser_requires_explicit_rgba) {
    for(auto text : { "#3366FF80", "#3366ff80", "#3366fF80" }) {
        const auto parsed = Color::TryParse(text);
        zassert_true(parsed.has_value());
        zassert_true(parsed->has_value());
        const auto& color = **parsed;
        zassert_equal(color.red, 0x33);
        zassert_equal(color.green, 0x66);
        zassert_equal(color.blue, 0xff);
        zassert_equal(color.alpha, 0x80);
    }

    auto parsed = Color::TryParse("#12345678");
    zassert_true(parsed.has_value());
    zassert_true((*parsed == Color { 0x12, 0x34, 0x56, 0x78 }));
    parsed = Color::TryParse("#FFFFFFFF");
    zassert_true(parsed.has_value());
    zassert_true((*parsed == Color { 255, 255, 255, 255 }));
}

ZTEST(widget_property, test_color_parser_distinguishes_transparent_override_from_reset) {
    const auto transparent = Color::TryParse("#00000000");
    zassert_true(transparent.has_value());
    zassert_true((*transparent == Color { 0, 0, 0, 0 }));
    const auto reset = Color::TryParse("");
    zassert_true(reset.has_value());
    zassert_false(reset->has_value());
}

ZTEST(widget_property, test_color_parser_reports_invalid_argument_for_malformed_input) {
    constexpr std::string_view invalid_values[] = {
        "#3366FF", "3366FF80", "#1234567", "#123456789", " #12345678", "#12345678 ",
        "#GG345678", "#12GG5678", "#1234GG78", "#123456GG", "#+1345678", "#-1345678", "#1 345678",
        "#0x345678", "#1234567#", std::string_view("#1234567\0", 9), std::string_view("#12345678\0", 10)
    };
    for(auto text : invalid_values) {
        const auto parsed = Color::TryParse(text);
        zassert_false(parsed.has_value(), "Accepted malformed color: %.*s", static_cast<int>(text.size()), text.data());
        zassert_true(parsed.error() == std::errc::invalid_argument);
    }
}
