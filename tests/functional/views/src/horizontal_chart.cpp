#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

#include <lvgl_private.h>
#include <zephyr/ztest.h>

#include <eerie_memory.hpp>

#include "domain/ui_domain/lvgl_lock.h"
#include "domain/ui_domain/models/widget_configuration.h"
#include "utilities/memory/memory_resource_manager.h"
#include "views/utilitites/frame.h"
#include "views/widgets/indicators/horizontal_chart_indicator/horizontal_chart_indicator.h"
#include "views/widgets/indicators/indicator_base.h"
#include "views/widgets/widget_context.h"

#include "views_test_support.h"

using namespace eerie_memory;

using eerie_leap::domain::ui_domain::ScopedLvglLock;
using eerie_leap::domain::ui_domain::models::WidgetConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetPropertyType;
using eerie_leap::domain::ui_domain::models::WidgetType;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::widgets::WidgetContext;
using eerie_leap::views::widgets::indicators::HorizontalChartIndicator;
using eerie_leap::views::widgets::indicators::HorizontalChartIndicatorType;
using eerie_leap::views::widgets::indicators::IndicatorBase;

namespace {

constexpr int32_t SCALE = IndicatorBase::LVGL_VALUE_SCALE;
constexpr int32_t POINTS = 5;

std::shared_ptr<Frame> MakeRoot() {
    return std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(100, false).SetHeight(100, false).Build());
}

// An unbound chart: the record's value is its first point, and Update() is the live source.
struct Chart {
    std::shared_ptr<Frame> root = MakeRoot();
    HorizontalChartIndicator indicator;

    Chart(HorizontalChartIndicatorType type, bool is_smoothed, double max = 100.0) :
        indicator(1, root, WidgetContext{}) {
        auto configuration = make_shared_pmr<WidgetConfiguration>(Mrm::GetExtPmr());
        configuration->type = WidgetType::IndicatorHorizontalChart;
        configuration->id = 1;
        configuration->properties[WidgetPropertyType::CHART_TYPE] = static_cast<int>(type);
        configuration->properties[WidgetPropertyType::CHART_POINT_COUNT] = POINTS;
        configuration->properties[WidgetPropertyType::MAX_VALUE] = max;
        configuration->properties[WidgetPropertyType::IS_SMOOTHED] = is_smoothed;
        indicator.Configure(configuration);
        zassert_equal(indicator.Render(), 0);
        indicator.OnActivated();
        lv_obj_update_layout(root->GetObject());
    }

    lv_obj_t* Viewport() const {
        return lv_obj_get_child(views_test::WidgetContent(indicator), 0);
    }
    lv_obj_t* Plot() const {
        return lv_obj_get_child(Viewport(), 0);
    }
    lv_chart_series_t* Series() const {
        return lv_chart_get_series_next(Plot(), nullptr);
    }
    int32_t Translate() const {
        return lv_obj_get_style_translate_x(Plot(), LV_PART_MAIN);
    }
    bool IsScrolling() {
        return lv_anim_get(&indicator, nullptr) != nullptr;
    }
    // The newest point, which the chart wrote last.
    int32_t Newest() const {
        const auto count = lv_chart_get_point_count(Plot());
        const auto next = lv_chart_get_x_start_point(Plot(), Series());

        return lv_chart_get_series_y_array(Plot(), Series())[(next + count - 1) % count];
    }
    // What the scroll moves by, from the geometry the box asks for.
    int32_t Step() const {
        const bool is_line = lv_chart_get_type(Plot()) == LV_CHART_TYPE_LINE;

        return static_cast<int32_t>(std::lround(
            static_cast<float>(lv_obj_get_width(Plot())) / static_cast<float>(is_line ? POINTS : POINTS + 1)
        ));
    }
};

void Tick(uint32_t ms) {
    lv_tick_inc(ms);
    lv_anim_refr_now();
}

void* Setup() {
    views_test::EnsureTestDisplay();

    return nullptr;
}

} // namespace

ZTEST_SUITE(horizontal_chart, NULL, Setup, NULL, views_test::CleanTestDisplay, NULL);

ZTEST(horizontal_chart, test_the_chart_sits_in_a_viewport_with_one_point_hidden_left_of_the_box) {
    ScopedLvglLock lock;
    for(const auto type : { HorizontalChartIndicatorType::Line, HorizontalChartIndicatorType::Bar }) {
        Chart chart(type, false);
        const bool is_line = type == HorizontalChartIndicatorType::Line;
        const int32_t width = lv_obj_get_content_width(chart.Viewport());
        zassert_true(width > 0);
        const auto extra = static_cast<int32_t>(
            std::lround(static_cast<float>(width) / static_cast<float>(is_line ? POINTS - 1 : POINTS))
        );

        zassert_equal(lv_obj_get_child_count(chart.Viewport()), 1U);
        zassert_equal(lv_chart_get_point_count(chart.Plot()), static_cast<uint32_t>(POINTS + 1));
        zassert_equal(lv_obj_get_width(chart.Plot()), width + extra);
        zassert_equal(lv_obj_get_x(chart.Plot()), -extra);
        zassert_equal(lv_obj_get_height(chart.Plot()), lv_obj_get_content_height(chart.Viewport()));
        zassert_false(lv_obj_is_scrollable(chart.Viewport()));
        zassert_false(lv_obj_is_clickable(chart.Viewport()));
        zassert_equal(chart.Translate(), 0);
    }
}

ZTEST(horizontal_chart, test_the_geometry_follows_the_box) {
    ScopedLvglLock lock;
    for(const auto type : { HorizontalChartIndicatorType::Line, HorizontalChartIndicatorType::Bar }) {
        Chart chart(type, true);
        const bool is_line = type == HorizontalChartIndicatorType::Line;
        const int32_t before = lv_obj_get_width(chart.Plot());

        chart.indicator.SetSizePx({ 200, 100 });
        lv_obj_update_layout(chart.root->GetObject());

        const int32_t width = lv_obj_get_content_width(chart.Viewport());
        const auto extra = static_cast<int32_t>(
            std::lround(static_cast<float>(width) / static_cast<float>(is_line ? POINTS - 1 : POINTS))
        );
        zassert_true(lv_obj_get_width(chart.Plot()) != before, "the chart did not follow the viewport");
        zassert_equal(lv_obj_get_width(chart.Plot()), width + extra);
        zassert_equal(lv_obj_get_x(chart.Plot()), -extra);
    }
}

ZTEST(horizontal_chart, test_values_sit_on_the_fixed_scale) {
    ScopedLvglLock lock;
    Chart chart(HorizontalChartIndicatorType::Line, false);

    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): LVGL has no getter for the axis range.
    const auto* lv_chart = reinterpret_cast<const lv_chart_t*>(chart.Plot());
    zassert_equal(lv_chart->ymin[LV_CHART_AXIS_PRIMARY_Y], 0);
    zassert_equal(lv_chart->ymax[LV_CHART_AXIS_PRIMARY_Y], SCALE);

    for(const auto& expected : { std::pair{ 0.0F, 0 }, std::pair{ 100.0F, SCALE }, std::pair{ 33.33F, 3333 },
            std::pair{ 250.0F, SCALE }, std::pair{ -5.0F, 0 } }) {
        chart.indicator.Update(expected.first);
        zassert_equal(chart.Newest(), expected.second, "value %g", static_cast<double>(expected.first));
    }
}

ZTEST(horizontal_chart, test_a_fractional_range_is_not_truncated) {
    ScopedLvglLock lock;
    Chart chart(HorizontalChartIndicatorType::Bar, false, 1.5);

    chart.indicator.Update(0.75F);
    zassert_equal(chart.Newest(), 5000);
    chart.indicator.Update(1.5F);
    zassert_equal(chart.Newest(), SCALE);
}

ZTEST(horizontal_chart, test_without_smoothing_the_chart_jumps) {
    ScopedLvglLock lock;
    for(const auto type : { HorizontalChartIndicatorType::Line, HorizontalChartIndicatorType::Bar }) {
        Chart chart(type, false);

        Tick(1000);
        chart.indicator.Update(40.0F);
        zassert_false(chart.IsScrolling());
        zassert_equal(chart.Translate(), 0);
        zassert_equal(chart.Newest(), 4000);
    }
}

ZTEST(horizontal_chart, test_a_smoothed_chart_scrolls_a_step_over_the_interval_between_values) {
    ScopedLvglLock lock;
    for(const auto type : { HorizontalChartIndicatorType::Line, HorizontalChartIndicatorType::Bar }) {
        Chart chart(type, true);

        // The record's value was the first point and is no scroll. The second value scrolls.
        zassert_false(chart.IsScrolling());
        Tick(1000);
        chart.indicator.Update(40.0F);
        zassert_true(chart.IsScrolling());
        zassert_equal(lv_anim_get_time(lv_anim_get(&chart.indicator, nullptr)), 1000U);
        const int32_t step = chart.Step();
        zassert_true(step > 0);
        zassert_equal(chart.Translate(), step);

        Tick(500);
        zassert_within(chart.Translate(), step / 2, 1);
        Tick(499);
        zassert_true(chart.Translate() <= 1 && chart.Translate() >= 0);
        Tick(1);
        zassert_equal(chart.Translate(), 0);
        zassert_false(chart.IsScrolling());
    }
}

ZTEST(horizontal_chart, test_an_early_value_cuts_the_scroll_short) {
    ScopedLvglLock lock;
    Chart chart(HorizontalChartIndicatorType::Line, true);

    Tick(1000);
    chart.indicator.Update(40.0F);
    Tick(300);
    chart.indicator.Update(60.0F);

    // The remainder snapped and a new scroll started; its duration moved a quarter of the way to the shorter interval.
    zassert_true(chart.IsScrolling());
    zassert_equal(chart.Translate(), chart.Step());
    zassert_equal(lv_anim_get_time(lv_anim_get(&chart.indicator, nullptr)), 825U);
    zassert_equal(chart.Newest(), 6000);

    Tick(825);
    zassert_equal(chart.Translate(), 0);
    zassert_false(chart.IsScrolling());
}

ZTEST(horizontal_chart, test_a_pause_completes_the_scroll_and_the_next_point_appends_without_one) {
    ScopedLvglLock lock;
    Chart chart(HorizontalChartIndicatorType::Line, true);

    Tick(1000);
    chart.indicator.Update(40.0F);
    Tick(200);
    zassert_true(chart.Translate() > 0);

    chart.indicator.OnDeactivated();
    zassert_false(chart.IsScrolling());
    zassert_equal(chart.Translate(), 0);

    // The replay on resume appends the stored value without a scroll, whatever came before the pause.
    chart.indicator.OnActivated();
    zassert_false(chart.IsScrolling());
    zassert_equal(chart.Translate(), 0);

    // Scrolling resumes with the next value, now that a point is there to measure from.
    Tick(1000);
    chart.indicator.Update(70.0F);
    zassert_true(chart.IsScrolling());
    zassert_equal(chart.Newest(), 7000);
}

ZTEST(horizontal_chart, test_a_long_gap_does_not_scroll) {
    ScopedLvglLock lock;
    Chart chart(HorizontalChartIndicatorType::Line, true);

    Tick(10001);
    chart.indicator.Update(40.0F);
    zassert_false(chart.IsScrolling());
    zassert_equal(chart.Translate(), 0);
    zassert_equal(chart.Newest(), 4000);

    // The gap left no rate behind: the next interval is taken as is, up to the limit.
    Tick(10000);
    chart.indicator.Update(50.0F);
    zassert_true(chart.IsScrolling());
    zassert_equal(lv_anim_get_time(lv_anim_get(&chart.indicator, nullptr)), 10000U);
}

ZTEST(horizontal_chart, test_the_translation_changes_only_when_the_pixel_does) {
    ScopedLvglLock lock;
    Chart chart(HorizontalChartIndicatorType::Line, true);

    Tick(1000);
    chart.indicator.Update(40.0F);
    int32_t changes = 0;
    int32_t last = chart.Translate();
    for(int i = 0; i < 100; i++) {
        Tick(10);
        if(chart.Translate() != last) {
            changes++;
            last = chart.Translate();
        }
    }

    zassert_true(changes <= chart.Step(), "%d changes for a step of %d px", changes, chart.Step());
}

ZTEST(horizontal_chart, test_destroying_a_scrolling_chart_leaves_no_animation_behind) {
    ScopedLvglLock lock;
    const auto count = lv_anim_count_running();
    {
        Chart chart(HorizontalChartIndicatorType::Bar, true);
        Tick(1000);
        chart.indicator.Update(40.0F);
        zassert_true(chart.IsScrolling());
    }

    zassert_equal(lv_anim_count_running(), count);
    Tick(2000);
    lv_refr_now(nullptr);
}
