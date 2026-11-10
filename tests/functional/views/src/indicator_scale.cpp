#include <memory>
#include <utility>

#include <zephyr/ztest.h>

#include <eerie_memory.hpp>

#include "domain/ui_domain/models/widget_configuration.h"
#include "utilities/memory/memory_resource_manager.h"
#include "views/utilitites/frame.h"
#include "views/widgets/indicators/arc_fill_indicator/arc_fill_indicator.h"
#include "views/widgets/indicators/bar_indicator/bar_indicator.h"
#include "views/widgets/widget_context.h"

#include "views_test_support.h"

using namespace eerie_memory;

using eerie_leap::domain::ui_domain::models::WidgetConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetDirection;
using eerie_leap::domain::ui_domain::models::WidgetPropertyType;
using eerie_leap::domain::ui_domain::models::WidgetType;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::widgets::WidgetContext;
using eerie_leap::views::widgets::indicators::ArcFillIndicator;
using eerie_leap::views::widgets::indicators::BarIndicator;
using eerie_leap::views::widgets::indicators::IndicatorBase;

namespace {

constexpr int32_t SCALE = IndicatorBase::LVGL_VALUE_SCALE;

std::shared_ptr<Frame> MakeRoot() {
    return std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(100, false).SetHeight(100, false).Build());
}

std::shared_ptr<WidgetConfiguration> MakeConfiguration(WidgetType type, double min, double max, double value) {
    auto configuration = make_shared_pmr<WidgetConfiguration>(Mrm::GetExtPmr());
    configuration->type = type;
    configuration->id = 1;
    configuration->properties[WidgetPropertyType::MIN_VALUE] = min;
    configuration->properties[WidgetPropertyType::MAX_VALUE] = max;
    configuration->properties[WidgetPropertyType::VALUE] = value;

    return configuration;
}

void* Setup() {
    views_test::EnsureTestDisplay();

    return nullptr;
}

} // namespace

ZTEST_SUITE(indicator_scale, NULL, Setup, NULL, views_test::CleanTestDisplay, NULL);

ZTEST(indicator_scale, test_a_bar_shows_a_fraction_of_a_unit) {
    // A range of 0..8 has nine whole positions; on the scale a quarter of a unit is 312 of 10000.
    BarIndicator indicator(1, MakeRoot(), WidgetContext{});
    indicator.Configure(MakeConfiguration(WidgetType::IndicatorBar, 0, 8, 4.25));
    zassert_equal(indicator.Render(), 0);
    indicator.OnActivated();
    auto* bar = lv_obj_get_child(views_test::WidgetContent(indicator), 0);

    zassert_equal(lv_bar_get_min_value(bar), 0);
    zassert_equal(lv_bar_get_max_value(bar), SCALE);
    zassert_between_inclusive(lv_bar_get_value(bar), 5312, 5313);
}

ZTEST(indicator_scale, test_a_bar_keeps_a_fractional_range) {
    // 0.0..1.5 used to truncate to 0..1.
    for(const auto& expected : { std::pair{ 0.0, 0 }, std::pair{ 0.375, 2500 }, std::pair{ 0.75, 5000 },
            std::pair{ 1.5, SCALE }, std::pair{ 3.0, SCALE }, std::pair{ -1.0, 0 } }) {
        BarIndicator indicator(1, MakeRoot(), WidgetContext{});
        indicator.Configure(MakeConfiguration(WidgetType::IndicatorBar, 0.0, 1.5, expected.first));
        zassert_equal(indicator.Render(), 0);
        indicator.OnActivated();
        auto* bar = lv_obj_get_child(views_test::WidgetContent(indicator), 0);

        zassert_equal(lv_bar_get_value(bar), expected.second, "value %g", expected.first);
    }
}

ZTEST(indicator_scale, test_a_range_not_starting_at_zero_maps_from_its_start) {
    BarIndicator indicator(1, MakeRoot(), WidgetContext{});
    indicator.Configure(MakeConfiguration(WidgetType::IndicatorBar, -50, 50, 25));
    zassert_equal(indicator.Render(), 0);
    indicator.OnActivated();
    auto* bar = lv_obj_get_child(views_test::WidgetContent(indicator), 0);

    zassert_equal(lv_bar_get_value(bar), 7500);
}

ZTEST(indicator_scale, test_a_bar_top_to_bottom_reverses_the_scale_not_the_units) {
    auto configuration = MakeConfiguration(WidgetType::IndicatorBar, 0, 8, 2);
    configuration->properties[WidgetPropertyType::DIRECTION] = static_cast<int>(WidgetDirection::TopToBottom);
    BarIndicator indicator(1, MakeRoot(), WidgetContext{});
    indicator.Configure(configuration);
    zassert_equal(indicator.Render(), 0);
    indicator.OnActivated();
    auto* bar = lv_obj_get_child(views_test::WidgetContent(indicator), 0);

    zassert_equal(lv_bar_get_min_value(bar), SCALE);
    zassert_equal(lv_bar_get_max_value(bar), 0);
    zassert_equal(lv_bar_get_value(bar), 2500);
}

ZTEST(indicator_scale, test_a_bar_with_an_empty_range_shows_nothing) {
    BarIndicator indicator(1, MakeRoot(), WidgetContext{});
    indicator.Configure(MakeConfiguration(WidgetType::IndicatorBar, 5, 5, 5));
    zassert_equal(indicator.Render(), 0);
    indicator.OnActivated();
    auto* bar = lv_obj_get_child(views_test::WidgetContent(indicator), 0);

    zassert_equal(lv_bar_get_value(bar), 0);
}

ZTEST(indicator_scale, test_an_arc_fill_moves_in_sub_unit_steps) {
    ArcFillIndicator indicator(1, MakeRoot(), WidgetContext{});
    indicator.Configure(MakeConfiguration(WidgetType::IndicatorArcFill, 0, 1.5, 1.0));
    zassert_equal(indicator.Render(), 0);
    indicator.OnActivated();
    auto* arc = lv_obj_get_child(views_test::WidgetContent(indicator), 0);

    zassert_equal(lv_arc_get_min_value(arc), 0);
    zassert_equal(lv_arc_get_max_value(arc), SCALE);
    zassert_between_inclusive(lv_arc_get_value(arc), 6666, 6667);
}

ZTEST(indicator_scale, test_a_smoothed_bar_eases_through_the_scale) {
    auto configuration = MakeConfiguration(WidgetType::IndicatorBar, 0, 8, 0);
    configuration->properties[WidgetPropertyType::IS_SMOOTHED] = true;
    BarIndicator indicator(1, MakeRoot(), WidgetContext{});
    indicator.Configure(configuration);
    zassert_equal(indicator.Render(), 0);
    indicator.OnActivated();
    auto* bar = lv_obj_get_child(views_test::WidgetContent(indicator), 0);

    // Nothing here is bound, so the first value shown, the record's, is a reading to ease from; patches ease and a
    // value shown at a time in between is not a multiple of an eighth of the bar.
    indicator.Update(8.0F);
    lv_tick_inc(100);
    lv_anim_refr_now();
    const auto displayed = lv_bar_get_value(bar);
    zassert_true(displayed > 0 && displayed < SCALE);
    zassert_not_equal(displayed % (SCALE / 8), 0, "between two whole units: %d", displayed);
}
