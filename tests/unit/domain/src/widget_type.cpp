#include <array>
#include <cstdint>

#include <zephyr/ztest.h>

#include "domain/ui_domain/models/widget_type.h"

using namespace eerie_leap::domain::ui_domain::models;

ZTEST_SUITE(widget_type, NULL, NULL, NULL, NULL, NULL);

ZTEST(widget_type, test_category_is_resolved_per_type) {
    zassert_equal(WidgetTypeHelpers::GetCategory(WidgetType::BasicIcon), WidgetCategory::Basic);
    zassert_equal(WidgetTypeHelpers::GetCategory(WidgetType::BasicLottie), WidgetCategory::Basic);
    zassert_equal(WidgetTypeHelpers::GetCategory(WidgetType::IndicatorDial), WidgetCategory::Indicator);
    zassert_equal(WidgetTypeHelpers::GetCategory(WidgetType::ControlSlider), WidgetCategory::Control);
    zassert_equal(WidgetTypeHelpers::GetCategory(WidgetType::None), WidgetCategory::None);
}

// It renders a setting read-only and drives nothing, so it must not be grouped
// with the widgets that write one.
ZTEST(widget_type, test_setting_backed_display_is_an_indicator) {
    zassert_equal(WidgetTypeHelpers::GetCategory(WidgetType::IndicatorSetting), WidgetCategory::Indicator);
    zassert_false(WidgetTypeHelpers::IsCategory(WidgetType::IndicatorSetting, WidgetCategory::Control));
}

// Control (3 << 16) shares bits with Basic (1 << 16) and Indicator (2 << 16), so
// a plain bitwise AND reports a control widget as being in either of them.
ZTEST(widget_type, test_control_type_is_not_reported_as_another_category) {
    zassert_true(WidgetTypeHelpers::IsCategory(WidgetType::ControlSlider, WidgetCategory::Control));
    zassert_false(WidgetTypeHelpers::IsCategory(WidgetType::ControlSlider, WidgetCategory::Basic));
    zassert_false(WidgetTypeHelpers::IsCategory(WidgetType::ControlSlider, WidgetCategory::Indicator));
}

ZTEST(widget_type, test_indicator_type_is_not_reported_as_basic) {
    zassert_true(WidgetTypeHelpers::IsCategory(WidgetType::IndicatorBar, WidgetCategory::Indicator));
    zassert_false(WidgetTypeHelpers::IsCategory(WidgetType::IndicatorBar, WidgetCategory::Basic));
    zassert_false(WidgetTypeHelpers::IsCategory(WidgetType::IndicatorBar, WidgetCategory::Control));
}

ZTEST(widget_type, test_types_are_unique) {
    constexpr std::array types = {
        WidgetType::BasicIcon,
        WidgetType::BasicArcIcon,
        WidgetType::BasicLottie,
        WidgetType::IndicatorArcFill,
        WidgetType::IndicatorDigital,
        WidgetType::IndicatorHorizontalChart,
        WidgetType::IndicatorSegmentArc,
        WidgetType::IndicatorDial,
        WidgetType::IndicatorBar,
        WidgetType::IndicatorSetting,
        WidgetType::ControlSlider,
        WidgetType::ControlToggle,
        WidgetType::ControlButton
    };

    for(size_t i = 0; i < types.size(); i++)
        for(size_t j = i + 1; j < types.size(); j++)
            zassert_not_equal(
                static_cast<uint32_t>(types[i]),
                static_cast<uint32_t>(types[j]),
                "Widget types at %zu and %zu collide.", i, j);
}
