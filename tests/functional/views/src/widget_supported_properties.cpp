#include <algorithm>
#include <memory>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/ui_domain/models/widget_property.h"
#include "domain/ui_domain/utilities/widget_property_validator.h"

#include "views/utilitites/frame.h"
#include "views/widgets/indicators/bar_indicator/bar_indicator.h"
#include "views/widgets/indicators/dial_indicator/dial_indicator.h"
#include "views/widgets/widget_context.h"
#include "views/widgets/widget_factory.h"
#include "views/widgets/basic/icon_widget/icon_widget.h"
#include "views/widgets/basic/arc_icon_widget/arc_icon_widget.h"

#include "views_test_support.h"

using eerie_leap::domain::ui_domain::models::WidgetPropertyType;
using eerie_leap::domain::ui_domain::models::IsValidWidgetPropertyType;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::widgets::WidgetContext;
using eerie_leap::views::widgets::indicators::BarIndicator;
using eerie_leap::views::widgets::indicators::DialIndicator;
using views_test::CleanTestDisplay;
using views_test::EnsureTestDisplay;

namespace {

std::shared_ptr<Frame> MakeRoot() {
    return std::make_shared<Frame>(Frame::CreateWrapped()
        .SetWidth(100, false)
        .SetHeight(100, false)
        .Build());
}

bool Supports(const std::vector<WidgetPropertyType>& types, WidgetPropertyType type) {
    return std::find(types.begin(), types.end(), type) != types.end();
}

void* SetUp() {
    EnsureTestDisplay();

    return nullptr;
}

} // namespace

ZTEST_SUITE(widget_supported_properties, NULL, SetUp, NULL, CleanTestDisplay, NULL);

// The point of the method: it answers "what can I configure here?" for a widget nothing has
// configured yet, which is the only state a configuration editor can ask from.
ZTEST(widget_supported_properties, test_a_widget_reports_its_properties_before_it_is_configured) {
    BarIndicator indicator(1, MakeRoot(), WidgetContext { });

    auto supported = indicator.GetSupportedProperties();

    zassert_true(Supports(supported, WidgetPropertyType::IS_ACTIVE));
    zassert_true(Supports(supported, WidgetPropertyType::OPACITY));
    zassert_true(Supports(supported, WidgetPropertyType::IS_VISIBLE));
    zassert_true(Supports(supported, WidgetPropertyType::IS_SMOOTHED));
    zassert_true(Supports(supported, WidgetPropertyType::MIN_VALUE));
    zassert_true(Supports(supported, WidgetPropertyType::MAX_VALUE));
    zassert_true(Supports(supported, WidgetPropertyType::VALUE));
    zassert_true(Supports(supported, WidgetPropertyType::DIRECTION));
}

ZTEST(widget_supported_properties, test_a_widget_does_not_report_another_widgets_properties) {
    BarIndicator indicator(1, MakeRoot(), WidgetContext { });

    auto supported = indicator.GetSupportedProperties();

    zassert_false(Supports(supported, WidgetPropertyType::FILE_PATH));
    zassert_false(Supports(supported, WidgetPropertyType::START_ANGLE));
    zassert_false(Supports(supported, WidgetPropertyType::SETTING_ID));
}

// The needle is a separately configured child, so its image properties belong only to it.
ZTEST(widget_supported_properties, test_a_dial_and_its_needle_report_separate_properties) {
    using eerie_leap::domain::ui_domain::models::IconType;
    using eerie_leap::views::widgets::basic::IconWidget;
    DialIndicator dial(1, MakeRoot(), WidgetContext { });
    IconWidget needle(2, MakeRoot(), WidgetContext { }, IconType::Image);

    auto supported = dial.GetSupportedProperties();
    const auto needle_supported = needle.GetSupportedProperties();

    zassert_true(Supports(supported, WidgetPropertyType::START_ANGLE));
    zassert_true(Supports(supported, WidgetPropertyType::END_ANGLE));
    zassert_true(Supports(supported, WidgetPropertyType::VALUE));
    zassert_equal(std::count(supported.begin(), supported.end(), WidgetPropertyType::CHILD_WIDGET_IDS), 1);
    zassert_false(Supports(needle_supported, WidgetPropertyType::CHILD_WIDGET_IDS));
    zassert_false(Supports(needle_supported, WidgetPropertyType::START_ANGLE));

    for(auto type : { WidgetPropertyType::FILE_PATH, WidgetPropertyType::IMG_WIDTH, WidgetPropertyType::IMG_HEIGHT,
                     WidgetPropertyType::POSITION_X, WidgetPropertyType::POSITION_Y }) {
        zassert_false(Supports(supported, type));
        zassert_true(Supports(needle_supported, type));
    }
    for(auto type : { WidgetPropertyType::ANCHOR_POINT_X, WidgetPropertyType::ANCHOR_POINT_Y }) {
        zassert_equal(std::count(supported.begin(), supported.end(), type), 1);
        zassert_equal(std::count(needle_supported.begin(), needle_supported.end(), type), 1);
    }
}

ZTEST(widget_supported_properties, test_every_reported_property_is_a_valid_type) {
    DialIndicator dial(1, MakeRoot(), WidgetContext { });

    for(auto type : dial.GetSupportedProperties()) {
        zassert_not_equal(type, WidgetPropertyType::NONE);
        zassert_true(IsValidWidgetPropertyType(type));
    }
}

ZTEST(widget_supported_properties, test_widgets_report_exact_color_roles) {
    using eerie_leap::domain::ui_domain::models::WidgetType;
    using eerie_leap::views::widgets::WidgetFactory;
    struct Expected {
        WidgetType type;
        std::vector<WidgetPropertyType> colors;
    };
    const auto primary = WidgetPropertyType::COLOR_PRIMARY_ACTIVE;
    const auto secondary = WidgetPropertyType::COLOR_SECONDARY_ACTIVE;
    const auto primary_inactive = WidgetPropertyType::COLOR_PRIMARY_INACTIVE;
    const auto secondary_inactive = WidgetPropertyType::COLOR_SECONDARY_INACTIVE;
    const Expected cases[] = {
        { WidgetType::IndicatorDigital, { primary } },
        { WidgetType::IndicatorSetting, { primary } },
        { WidgetType::IndicatorHorizontalChart, { primary } },
        { WidgetType::IndicatorArcFill, { primary, secondary } },
        { WidgetType::IndicatorBar, { primary, secondary } },
        { WidgetType::IndicatorSegmentArc, { primary, primary_inactive } },
        { WidgetType::IndicatorDial, {} },
        { WidgetType::ControlButton, { primary, primary_inactive, secondary, secondary_inactive } },
        { WidgetType::ControlToggle, { primary, primary_inactive, secondary, secondary_inactive } },
        { WidgetType::ControlSlider, { primary, primary_inactive, secondary, secondary_inactive,
            WidgetPropertyType::COLOR_TERTIARY_ACTIVE, WidgetPropertyType::COLOR_TERTIARY_INACTIVE } }
    };
    for(const auto& expected : cases) {
        auto widget = WidgetFactory::GetInstance().CreateWidget(expected.type, 1, MakeRoot(), WidgetContext{});
        const auto supported = widget->GetSupportedProperties();
        for(auto property : eerie_leap::domain::ui_domain::utilities::WidgetPropertyValidator::color_properties) {
            zassert_equal(Supports(supported, property), Supports(expected.colors, property),
            "widget %d property %d", static_cast<int>(expected.type), static_cast<int>(property));
        }
        for(auto property : { WidgetPropertyType::IS_ACTIVE, WidgetPropertyType::IS_VISIBLE, WidgetPropertyType::OPACITY,
                             WidgetPropertyType::ANIMATION_TYPE, WidgetPropertyType::IS_ANIMATION_ACTIVE,
                             WidgetPropertyType::ANIMATION_DURATION_MS })
            zassert_true(Supports(supported, property));
    }
}

ZTEST(widget_supported_properties, test_icon_wrappers_report_only_selected_icon_colors) {
    using eerie_leap::domain::ui_domain::models::IconType;
    using eerie_leap::views::widgets::basic::IconWidget;
    using eerie_leap::views::widgets::basic::ArcIconWidget;
    for(auto type : { IconType::Label, IconType::Rectangle, IconType::TriangleIsosceles,
                     IconType::TriangleRight, IconType::Oval, IconType::Line, IconType::Image }) {
        IconWidget basic(1, MakeRoot(), WidgetContext{}, type);
        ArcIconWidget arc(2, MakeRoot(), WidgetContext{}, type);
        for(const auto* widget : { static_cast<const IconWidget*>(&basic), static_cast<const IconWidget*>(&arc) }) {
            const auto supported = widget->GetSupportedProperties();
            for(auto property : { WidgetPropertyType::ANIMATION_TYPE, WidgetPropertyType::IS_ANIMATION_ACTIVE,
                                 WidgetPropertyType::ANIMATION_DURATION_MS })
                zassert_equal(std::count(supported.begin(), supported.end(), property), 1);
            zassert_equal(Supports(supported, WidgetPropertyType::COLOR_PRIMARY_ACTIVE), type != IconType::Image);
            zassert_equal(Supports(supported, WidgetPropertyType::COLOR_SECONDARY_ACTIVE), type == IconType::Label);
            for(auto property : { WidgetPropertyType::COLOR_PRIMARY_INACTIVE, WidgetPropertyType::COLOR_SECONDARY_INACTIVE,
                                 WidgetPropertyType::COLOR_TERTIARY_ACTIVE, WidgetPropertyType::COLOR_TERTIARY_INACTIVE })
                zassert_false(Supports(supported, property));
        }
    }
}
