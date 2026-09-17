#include <algorithm>
#include <memory>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/ui_domain/models/widget_property.h"

#include "views/utilitites/frame.h"
#include "views/widgets/indicators/bar_indicator/bar_indicator.h"
#include "views/widgets/indicators/dial_indicator/dial_indicator.h"
#include "views/widgets/widget_context.h"

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

// The dial builds an IconWidget for its needle and declares it as a dependency, so those
// properties are configurable on a dial even though the dial never reads them itself.
ZTEST(widget_supported_properties, test_a_widget_reports_the_properties_of_its_dependencies) {
    DialIndicator dial(1, MakeRoot(), WidgetContext { });

    auto supported = dial.GetSupportedProperties();

    zassert_true(Supports(supported, WidgetPropertyType::START_ANGLE));
    zassert_true(Supports(supported, WidgetPropertyType::END_ANGLE));
    zassert_true(Supports(supported, WidgetPropertyType::VALUE));

    zassert_true(Supports(supported, WidgetPropertyType::FILE_PATH));
    zassert_true(Supports(supported, WidgetPropertyType::IMG_WIDTH));
    zassert_true(Supports(supported, WidgetPropertyType::IMG_HEIGHT));
    zassert_true(Supports(supported, WidgetPropertyType::PIVOT_X));
    zassert_true(Supports(supported, WidgetPropertyType::PIVOT_Y));
    zassert_true(Supports(supported, WidgetPropertyType::POSITION_X));
    zassert_true(Supports(supported, WidgetPropertyType::POSITION_Y));
}

ZTEST(widget_supported_properties, test_every_reported_property_is_a_valid_type) {
    DialIndicator dial(1, MakeRoot(), WidgetContext { });

    for(auto type : dial.GetSupportedProperties()) {
        zassert_not_equal(type, WidgetPropertyType::NONE);
        zassert_true(IsValidWidgetPropertyType(type));
    }
}
