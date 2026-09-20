#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/sensor_domain/event_bus/sensor_events_channel.h"
#include "domain/ui_domain/models/icon_type.h"
#include "domain/ui_domain/models/widget_fill_mode.h"
#include "event_bus/event_channel_id.h"
#include "event_bus/event_channels.h"
#include "views/themes/default_theme.h"
#include "views/widgets/widget_base.h"
#include "views/widgets/widget_factory.h"
#include "views/widgets/indicators/horizontal_chart_indicator/horizontal_chart_indicator.h"

#include "views_test_support.h"

using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::domain::sensor_domain::event_bus;
using namespace eerie_leap::views::widgets;
using namespace eerie_leap::views::themes;
using eerie_leap::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::views::utilitites::Frame;

namespace {

constexpr auto primary = WidgetPropertyType::COLOR_PRIMARY_ACTIVE;
constexpr auto inactive = WidgetPropertyType::COLOR_PRIMARY_INACTIVE;
constexpr auto opacity = WidgetPropertyType::OPACITY;
constexpr int extent = 64;
// Intermediate layers use RGB565. Allow rounding across color, widget, and ancestor composition.
constexpr int rounding = 10;

class AlphaTheme : public DefaultTheme {
    uint8_t alpha_;
public:
    explicit AlphaTheme(uint8_t alpha) : alpha_(alpha) {}
    LvglColor GetPrimaryColor() const override { return LvglColor(0xFFFFFF, alpha_); }
    LvglColor GetSecondaryColor() const override { return LvglColor(0xFFFFFF, alpha_); }
    LvglColor GetAccentColor() const override { return LvglColor(0xFFFFFF, alpha_); }
};

void Theme(int alpha) {
    ThemeManager::GetInstance().SetTheme(std::make_shared<AlphaTheme>(alpha));
}

void Publish(WidgetPropertyType target, const EventData& value) {
    SensorEventsChannel::GetInstance().Publish({
        .source_id = 0,
        .type = SensorEventType::DataUpdated,
        .payload = {
            { SensorPayloadType::SensorId, static_cast<int>(target) },
            { SensorPayloadType::Value, value }
        }
    });
}

std::string White(int alpha) {
    char rgba[10];
    std::snprintf(rgba, sizeof(rgba), "#FFFFFF%02X", alpha);
    return rgba;
}

std::shared_ptr<WidgetConfiguration> Configuration(WidgetType type, IconType icon = IconType::Oval) {
    auto configuration = std::make_shared<WidgetConfiguration>(std::allocator_arg, std::pmr::get_default_resource());
    configuration->type = type;
    if(type == WidgetType::BasicIcon)
        configuration->properties[WidgetPropertyType::ICON_TYPE] = static_cast<int>(icon);
    return configuration;
}

struct Scene {
    std::shared_ptr<Frame> root = std::make_shared<Frame>(
        Frame::CreateWrapped().SetWidth(extent, true).SetHeight(extent, true).Build());
    std::shared_ptr<Frame> parent = std::make_shared<Frame>(
        Frame::CreateWrapped(root->GetObject()).SetWidth(extent, true).SetHeight(extent, true).Build());
    std::unique_ptr<IWidget> widget;

    explicit Scene(std::shared_ptr<WidgetConfiguration> configuration) {
        lv_obj_set_style_bg_color(root->GetObject(), lv_color_black(), 0);
        lv_obj_set_style_bg_opa(root->GetObject(), LV_OPA_COVER, 0);
        // Discover icon-specific properties before binding; only supported keys get subscriptions.
        widget = WidgetFactory::GetInstance().CreateWidget(configuration, parent, WidgetContext{});
        for(auto target : widget->GetSupportedProperties()) {
            configuration->bindings.push_back(PropertyBinding {
                .target = target,
                .channel = EventChannelId::Sensors,
                .event_type = std::to_underlying(SensorEventType::DataUpdated),
                .payload_key = std::to_underlying(SensorPayloadType::Value),
                .selector_key = std::to_underlying(SensorPayloadType::SensorId),
                .selector_value = static_cast<int>(target)
            });
        }
        widget->Configure(configuration);
        zassert_equal(widget->Render(), 0);
        widget->OnActivated();
        lv_obj_update_layout(root->GetObject());
    }

    lv_obj_t* Outer() const { return widget->GetContainer()->GetObject(); }
    lv_obj_t* Inner() const { return lv_obj_get_child(views_test::WidgetContent(*widget), 0); }
    WidgetBase& Base() const { return *dynamic_cast<WidgetBase*>(widget.get()); }

    std::unique_ptr<lv_draw_buf_t, decltype(&lv_draw_buf_destroy)> Snapshot() const {
        lv_obj_update_layout(root->GetObject());
        std::unique_ptr<lv_draw_buf_t, decltype(&lv_draw_buf_destroy)> snapshot(
            lv_snapshot_take(root->GetObject(), LV_COLOR_FORMAT_ARGB8888), lv_draw_buf_destroy);
        zassert_not_null(snapshot);
        zassert_equal(snapshot->header.w, extent);
        zassert_equal(snapshot->header.h, extent);
        return snapshot;
    }

    std::vector<uint8_t> RedPixels() const {
        auto snapshot = Snapshot();
        std::vector<uint8_t> result(extent * extent);
        for(int y = 0; y < extent; ++y) {
            auto* pixels = reinterpret_cast<const lv_color32_t*>(snapshot->data + y * snapshot->header.stride);
            for(int x = 0; x < extent; ++x)
                result[y * extent + x] = pixels[x].red;
        }
        return result;
    }

    void CheckBrightness(int expected) const {
        const auto pixels = RedPixels();
        const auto brightest = *std::max_element(pixels.begin(), pixels.end());
        zassert_within(brightest, expected, rounding, "expected=%d actual=%d", expected, brightest);
        if(expected == 0)
            zassert_equal(brightest, 0);
    }
};

void CheckAlphaMatrix(Scene& scene) {
    for(bool explicit_color : { false, true }) {
        for(int alpha : { 0, 128, 255 }) {
            Publish(opacity, 255);
            // Explicit alpha replaces even a fully transparent theme alpha.
            Theme(explicit_color ? 0 : alpha);
            Publish(primary, explicit_color ? White(alpha) : std::string{});
            for(int widget_alpha : { 255, 128, 0 }) {
                Publish(opacity, widget_alpha);
                scene.CheckBrightness((alpha * widget_alpha + 127) / 255);
                zassert_equal(scene.Base().IsProcessingEligible(), widget_alpha > 0);
            }
        }
    }
    Publish(opacity, 128);
    Publish(primary, White(128));
    lv_obj_set_style_opa_layered(scene.parent->GetObject(), 128, 0);
    scene.CheckBrightness(32);
    zassert_equal(lv_obj_get_style_opa_layered(scene.Outer(), LV_PART_MAIN), 128);
    lv_obj_set_style_opa_layered(scene.parent->GetObject(), 255, 0);
    Publish(opacity, 255);
}

void* Setup() {
    views_test::EnsureTestDisplay();
    eerie_leap::event_bus::InitializeEventChannels();
    return nullptr;
}

void Before(void*) { Theme(255); }

void Clean(void* fixture) {
    ThemeManager::GetInstance().SetTheme(std::make_shared<DefaultTheme>());
    views_test::CleanTestDisplay(fixture);
}

} // namespace

ZTEST_SUITE(widget_alpha, NULL, Setup, Before, Clean, NULL);

ZTEST(widget_alpha, test_chart_pixels_compose_series_alpha_once_and_preserve_history) {
    using eerie_leap::views::widgets::indicators::HorizontalChartIndicatorType;
    for(auto type : { HorizontalChartIndicatorType::Line, HorizontalChartIndicatorType::Bar }) {
        auto configuration = Configuration(WidgetType::IndicatorHorizontalChart);
        configuration->properties[WidgetPropertyType::CHART_TYPE] = static_cast<int>(type);
        configuration->properties[WidgetPropertyType::CHART_POINT_COUNT] = 4;
        Scene scene(configuration);
        auto* chart = scene.Inner();
        auto* series = lv_chart_get_series_next(chart, nullptr);
        for(int value : { 30, 70, 40, 80 })
            Publish(WidgetPropertyType::VALUE, value);
        auto* samples = lv_chart_get_y_array(chart, series);
        const std::vector<int32_t> before(samples, samples + 4);
        const auto start = lv_chart_get_x_start_point(chart, series);
        CheckAlphaMatrix(scene);
        zassert_equal(scene.Inner(), chart);
        zassert_equal(lv_chart_get_series_next(chart, nullptr), series);
        zassert_equal(lv_chart_get_y_array(chart, series), samples);
        zassert_true(std::equal(before.begin(), before.end(), samples));
        zassert_equal(lv_chart_get_x_start_point(chart, series), start);
        Publish(primary, White(0));
        Publish(WidgetPropertyType::VALUE, 55);
        zassert_equal(lv_chart_get_x_start_point(chart, series), (start + 1) % 4);
        zassert_equal(samples[start], 55);
        scene.CheckBrightness(0);
        Publish(primary, White(255));
        scene.CheckBrightness(255);
    }
}

ZTEST(widget_alpha, test_shape_masks_compose_coverage_and_alpha_without_rasterizing_again) {
    for(auto type : { IconType::Rectangle, IconType::TriangleIsosceles, IconType::TriangleRight,
                     IconType::Oval, IconType::Line }) {
        for(auto fill : { WidgetFillMode::Filled, WidgetFillMode::Outline }) {
            auto configuration = Configuration(WidgetType::BasicIcon, type);
            configuration->properties[WidgetPropertyType::WIDTH_PX] = 43;
            configuration->properties[WidgetPropertyType::HEIGHT_PX] = 36;
            configuration->properties[WidgetPropertyType::STROKE_PX] = 3;
            if(type != IconType::Line)
                configuration->properties[WidgetPropertyType::FILL_MODE] = static_cast<int>(fill);
            if(type == IconType::Rectangle)
                configuration->properties[WidgetPropertyType::CORNER_RAD_PX] = 7;
            Scene scene(configuration);
            auto* object = scene.Inner();
            const auto* mask = static_cast<const lv_image_dsc_t*>(lv_image_get_src(object));
            zassert_not_null(mask);
            const auto* data = mask->data;
            const std::vector<uint8_t> before(data, data + mask->data_size);
            lv_area_t bounds;
            lv_obj_get_coords(object, &bounds);
            CheckAlphaMatrix(scene);
            Publish(primary, White(128));
            Publish(opacity, 128);
            const auto pixels = scene.RedPixels();
            size_t antialiased = 0;
            for(uint32_t y = 0; y < mask->header.h; ++y) {
                for(uint32_t x = 0; x < mask->header.w; ++x) {
                    const int coverage = data[y * mask->header.stride + x];
                    antialiased += coverage > 0 && coverage < 255;
                    const int expected = (coverage * 128 * 128 + 32512) / (255 * 255);
                    const int actual = pixels[(bounds.y1 + y) * extent + bounds.x1 + x];
                    zassert_within(actual, expected, rounding, "shape=%d coverage=%d actual=%d expected=%d",
                        static_cast<int>(type), coverage, actual, expected);
                }
            }
            zassert_true(antialiased > 0);
            zassert_equal(scene.Inner(), object);
            zassert_equal(lv_image_get_src(object), mask);
            zassert_equal(mask->data, data);
            zassert_true(std::equal(before.begin(), before.end(), mask->data));
            lv_area_t after;
            lv_obj_get_coords(object, &after);
            zassert_equal(bounds.x1, after.x1);
            zassert_equal(bounds.y1, after.y1);
            zassert_equal(bounds.x2, after.x2);
            zassert_equal(bounds.y2, after.y2);
        }
    }
}

ZTEST(widget_alpha, test_static_oval_composes_theme_and_explicit_alpha) {
    Scene scene(Configuration(WidgetType::BasicIcon));
    const auto animations = lv_anim_count_running();
    CheckAlphaMatrix(scene);
    Publish(primary, White(0));
    Publish(WidgetPropertyType::POSITION_X, 5);
    zassert_equal(lv_obj_get_style_x(scene.Inner(), LV_PART_MAIN), 5);
    zassert_true(scene.Base().IsProcessingEligible());
    zassert_equal(lv_anim_count_running(), animations);
}

ZTEST(widget_alpha, test_layered_ancestor_opacity_restores_only_the_latest_hidden_sample) {
    Scene scene(Configuration(WidgetType::IndicatorHorizontalChart));
    auto* chart = scene.Inner();
    auto* series = lv_chart_get_series_next(chart, nullptr);
    auto* samples = lv_chart_get_y_array(chart, series);
    const auto start = lv_chart_get_x_start_point(chart, series);
    const auto previous = samples[start];
    lv_obj_set_style_opa_layered(scene.parent->GetObject(), 0, 0);
    zassert_false(scene.Base().IsProcessingEligible());
    Publish(WidgetPropertyType::VALUE, 40);
    Publish(WidgetPropertyType::VALUE, 80);
    Publish(primary, White(128));
    zassert_equal(lv_chart_get_x_start_point(chart, series), start);
    zassert_equal(samples[start], previous);
    lv_obj_set_style_opa_layered(scene.parent->GetObject(), 128, 0);
    lv_refr_now(nullptr);
    zassert_true(scene.Base().IsProcessingEligible());
    zassert_equal(lv_chart_get_x_start_point(chart, series), (start + 1) % lv_chart_get_point_count(chart));
    zassert_equal(samples[start], 80);
    zassert_equal(lv_obj_get_style_opa_layered(chart, LV_PART_MAIN), 128);
}

ZTEST(widget_alpha, test_segment_pixels_use_resolved_alpha_for_both_displayed_states) {
    Scene scene(Configuration(WidgetType::IndicatorSegmentArc));
    // Isolate one stroke: neighboring segments overlap near the center in this small fixture.
    const auto count = lv_obj_get_child_count(views_test::WidgetContent(*scene.widget));
    for(uint32_t i = 0; i + 1 < count; ++i)
        lv_obj_add_flag(lv_obj_get_child(views_test::WidgetContent(*scene.widget), i), LV_OBJ_FLAG_HIDDEN);
    Publish(WidgetPropertyType::VALUE, 100);
    CheckAlphaMatrix(scene);
    Publish(primary, White(0));
    Publish(WidgetPropertyType::VALUE, 0);
    scene.CheckBrightness(0);
    for(int alpha : { 0, 128, 255 }) {
        Publish(opacity, 255);
        Publish(inactive, White(alpha));
        Theme(255 - alpha);
        for(int widget_alpha : { 255, 128, 0 }) {
            Publish(opacity, widget_alpha);
            scene.CheckBrightness((alpha * widget_alpha + 127) / 255);
        }
    }
    Publish(opacity, 255);
    Publish(inactive, std::string{});
    scene.CheckBrightness(0);
}

ZTEST(widget_alpha, test_segment_animation_recolors_displayed_state_without_jumping_to_target) {
    auto configuration = Configuration(WidgetType::IndicatorSegmentArc);
    configuration->properties[WidgetPropertyType::IS_SMOOTHED] = true;
    configuration->properties[inactive] = std::pmr::string("#0000FF40");
    Scene scene(configuration);
    Publish(WidgetPropertyType::VALUE, 100);
    lv_tick_inc(400);
    lv_anim_refr_now();
    auto* animation = lv_anim_get(&scene.Base(), nullptr);
    zassert_not_null(animation);
    const auto count = lv_obj_get_child_count(views_test::WidgetContent(*scene.widget));
    std::vector<lv_obj_t*> segments;
    std::vector<bool> active;
    for(uint32_t i = 0; i < count; ++i) {
        auto* segment = lv_obj_get_child(views_test::WidgetContent(*scene.widget), i);
        segments.push_back(segment);
        active.push_back(lv_color_to_u32(lv_obj_get_style_arc_color(segment, LV_PART_INDICATOR))
            == lv_color_to_u32(lv_color_white()));
    }
    zassert_true(active.front());
    zassert_false(active.back());
    Theme(128);
    for(uint32_t i = 0; i < count; ++i)
        zassert_equal(lv_obj_get_style_arc_opa(segments[i], LV_PART_INDICATOR), active[i] ? 128 : 64);
    Publish(primary, std::string("#FF000020"));
    zassert_equal(lv_anim_get(&scene.Base(), nullptr), animation);
    for(uint32_t i = 0; i < count; ++i) {
        zassert_equal(lv_obj_get_child(views_test::WidgetContent(*scene.widget), i), segments[i]);
        zassert_equal(lv_obj_get_style_arc_opa(segments[i], LV_PART_INDICATOR), active[i] ? 32 : 64);
    }
    Publish(opacity, 0);
    zassert_is_null(lv_anim_get(&scene.Base(), nullptr));
    Publish(WidgetPropertyType::VALUE, 50);
    Publish(primary, std::string("#00FF0080"));
    Theme(0);
    for(uint32_t i = 0; i < count; ++i)
        zassert_equal(lv_obj_get_style_arc_opa(segments[i], LV_PART_INDICATOR), active[i] ? 32 : 64);
    Publish(opacity, 128);
    lv_tick_inc(5000);
    lv_anim_refr_now();
    zassert_equal(lv_obj_get_style_opa_layered(scene.Outer(), LV_PART_MAIN), 128);
    zassert_equal(lv_obj_get_style_arc_opa(segments.front(), LV_PART_INDICATOR), 128);
    zassert_equal(lv_obj_get_style_arc_opa(segments.back(), LV_PART_INDICATOR), 64);
    zassert_is_null(lv_anim_get(&scene.Base(), nullptr));
}

ZTEST(widget_alpha, test_fill_and_track_alpha_remain_independent_under_widget_fade) {
    auto configuration = Configuration(WidgetType::IndicatorBar);
    configuration->properties[WidgetPropertyType::VALUE] = 50.0;
    configuration->properties[primary] = std::pmr::string("#FF000080");
    configuration->properties[WidgetPropertyType::COLOR_SECONDARY_ACTIVE] = std::pmr::string("#0000FF80");
    configuration->properties[opacity] = 128;
    Scene scene(configuration);
    auto snapshot = scene.Snapshot();
    auto* pixels = reinterpret_cast<const lv_color32_t*>(snapshot->data + 32 * snapshot->header.stride);
    zassert_within(pixels[16].red, 64, rounding);
    zassert_within(pixels[16].blue, 32, rounding);
    zassert_equal(pixels[48].red, 0);
    zassert_within(pixels[48].blue, 64, rounding);
    zassert_equal(lv_bar_get_value(scene.Inner()), 50);
}

ZTEST(widget_alpha, test_control_style_changes_preserve_values_states_and_event_callbacks) {
    for(auto type : { WidgetType::ControlSlider, WidgetType::ControlToggle, WidgetType::ControlButton }) {
        auto configuration = Configuration(type);
        configuration->properties[WidgetPropertyType::MAX_VALUE] = 100.0;
        configuration->properties[WidgetPropertyType::VALUE] = type == WidgetType::ControlToggle
            ? ConfigValue{true} : ConfigValue{42.0};
        Scene scene(configuration);
        auto* object = scene.Inner();
        lv_obj_add_state(object, LV_STATE_PRESSED);
        lv_obj_send_event(object, LV_EVENT_PRESSED, nullptr);
        const auto state = lv_obj_get_state(object);
        const auto callbacks = lv_obj_get_event_count(object);
        const auto supported = scene.widget->GetSupportedProperties();
        for(int alpha : { 0, 128, 255 }) {
            Theme(alpha);
            for(auto property : { primary, inactive, WidgetPropertyType::COLOR_SECONDARY_ACTIVE,
                                 WidgetPropertyType::COLOR_SECONDARY_INACTIVE,
                                 WidgetPropertyType::COLOR_TERTIARY_ACTIVE,
                                 WidgetPropertyType::COLOR_TERTIARY_INACTIVE }) {
                if(std::find(supported.begin(), supported.end(), property) != supported.end())
                    Publish(property, White(alpha));
            }
            Publish(opacity, 128);
            zassert_equal(scene.Inner(), object);
            zassert_equal(lv_obj_get_state(object), state);
            zassert_equal(lv_obj_get_event_count(object), callbacks);
            if(type == WidgetType::ControlSlider)
                zassert_equal(lv_slider_get_value(object), 42);
            if(type == WidgetType::ControlToggle)
                zassert_true(lv_obj_has_state(object, LV_STATE_CHECKED));
        }
    }
}
