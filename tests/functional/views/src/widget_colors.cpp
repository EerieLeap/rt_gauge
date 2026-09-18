#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/sensor_domain/event_bus/sensor_events_channel.h"
#include "event_bus/event_channel_id.h"
#include "event_bus/event_channels.h"
#include "views/themes/default_theme.h"
#include "views/themes/dark_theme.h"
#include "views/themes/dark_bw_theme.h"
#include "views/widgets/widget_factory.h"
#include "views/widgets/widget_base.h"
#include "views/widgets/basic/icon_widget/icon_widget.h"
#include "views/widgets/basic/icons/icon_factory.h"
#include "views/widgets/indicators/horizontal_chart_indicator/horizontal_chart_indicator.h"

#include "views_test_support.h"

using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::domain::sensor_domain::event_bus;
using namespace eerie_leap::views::widgets;
using namespace eerie_leap::views::themes;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::EventData;

namespace {

constexpr auto primary = WidgetPropertyType::COLOR_PRIMARY_ACTIVE;
constexpr auto primary_inactive = WidgetPropertyType::COLOR_PRIMARY_INACTIVE;
constexpr auto secondary = WidgetPropertyType::COLOR_SECONDARY_ACTIVE;
constexpr auto secondary_inactive = WidgetPropertyType::COLOR_SECONDARY_INACTIVE;
constexpr auto tertiary = WidgetPropertyType::COLOR_TERTIARY_ACTIVE;
constexpr auto tertiary_inactive = WidgetPropertyType::COLOR_TERTIARY_INACTIVE;

class TranslucentTheme : public DefaultTheme {
public:
    LvglColor GetPrimaryColor() const override { return LvglColor(0x123456, 128); }
    LvglColor GetSecondaryColor() const override { return LvglColor(0x345678, 96); }
    LvglColor GetSurfaceColor() const override { return LvglColor(0x56789A, 64); }
    LvglColor GetAccentColor() const override { return LvglColor(0x789ABC, 32); }
};

std::shared_ptr<WidgetConfiguration> Configuration(WidgetType type, IconType icon = IconType::Dot) {
    auto configuration = std::make_shared<WidgetConfiguration>(std::allocator_arg, std::pmr::get_default_resource());
    configuration->id = 1;
    configuration->type = type;
    if(type == WidgetType::BasicIcon || type == WidgetType::BasicArcIcon)
        configuration->properties[WidgetPropertyType::ICON_TYPE] = static_cast<int>(icon);
    for(auto target : { primary, primary_inactive, secondary, secondary_inactive, tertiary, tertiary_inactive,
                       WidgetPropertyType::IS_VISIBLE, WidgetPropertyType::IS_ACTIVE, WidgetPropertyType::OPACITY,
                       WidgetPropertyType::VALUE }) {
        configuration->bindings.push_back(PropertyBinding {
            .target = target,
            .channel = EventChannelId::Sensors,
            .event_type = std::to_underlying(SensorEventType::DataUpdated),
            .payload_key = std::to_underlying(SensorPayloadType::Value),
            .selector_key = std::to_underlying(SensorPayloadType::SensorId),
            .selector_value = static_cast<int>(target)
        });
    }
    return configuration;
}

std::unique_ptr<IWidget> Render(std::shared_ptr<WidgetConfiguration> configuration) {
    auto root = std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(100, false).SetHeight(100, false).Build());
    auto widget = WidgetFactory::GetInstance().CreateWidget(configuration, root, WidgetContext{});
    zassert_equal(widget->Render(), 0);
    widget->OnActivated();
    return widget;
}

lv_obj_t* Inner(const IWidget& widget) {
    return widget.GetContainer()->GetChild()->GetObject();
}

void Publish(WidgetPropertyType type, const EventData& value) {
    SensorEventsChannel::GetInstance().Publish({
        .source_id = 0,
        .type = SensorEventType::DataUpdated,
        .payload = {
            { SensorPayloadType::SensorId, static_cast<int>(type) },
            { SensorPayloadType::Value, value }
        }
    });
}

void Color(WidgetPropertyType type, const char* value) {
    Publish(type, std::string(value));
}

void CheckColor(lv_color_t actual, LvglColor expected) {
    zassert_equal(lv_color_to_u32(actual), lv_color_to_u32(expected.ToLvColor()));
}

void CheckBackground(lv_obj_t* object, lv_part_t part, LvglColor expected) {
    CheckColor(lv_obj_get_style_bg_color(object, part), expected);
    zassert_equal(lv_obj_get_style_bg_opa(object, part), expected.ToLvOpa());
}

void CheckText(lv_obj_t* object, LvglColor expected) {
    CheckColor(lv_obj_get_style_text_color(object, LV_PART_MAIN), expected);
    zassert_equal(lv_obj_get_style_text_opa(object, LV_PART_MAIN), expected.ToLvOpa());
}

void CheckArc(lv_obj_t* object, lv_part_t part, LvglColor expected) {
    CheckColor(lv_obj_get_style_arc_color(object, part), expected);
    zassert_equal(lv_obj_get_style_arc_opa(object, part), expected.ToLvOpa());
}

void* Setup() {
    views_test::EnsureTestDisplay();
    eerie_leap::event_bus::InitializeEventChannels();
    return nullptr;
}

void Before(void*) {
    ThemeManager::GetInstance().SetTheme(std::make_shared<TranslucentTheme>());
}

void Clean(void* fixture) {
    ThemeManager::GetInstance().SetTheme(std::make_shared<DefaultTheme>());
    views_test::CleanTestDisplay(fixture);
}

} // namespace

ZTEST_SUITE(widget_colors, NULL, Setup, Before, Clean, NULL);

ZTEST(widget_colors, test_whole_widget_opacity_survives_render_theme_and_configuration_replay) {
    for(auto type : { WidgetType::BasicIcon, WidgetType::BasicArcIcon, WidgetType::IndicatorDigital,
                     WidgetType::IndicatorSetting, WidgetType::IndicatorHorizontalChart, WidgetType::IndicatorArcFill,
                     WidgetType::IndicatorBar, WidgetType::IndicatorSegmentArc, WidgetType::ControlButton,
                     WidgetType::ControlSlider, WidgetType::ControlToggle }) {
        auto configuration = Configuration(type);
        configuration->bindings.clear();
        configuration->properties[WidgetPropertyType::OPACITY] = 128;
        auto widget = Render(configuration);
        auto* outer = widget->GetContainer()->GetObject();
        auto* inner = lv_obj_get_child(outer, 0);
        zassert_equal(lv_obj_get_style_opa_layered(outer, LV_PART_MAIN), 128);
        zassert_equal(lv_obj_get_style_opa_layered(inner, LV_PART_MAIN), type == WidgetType::IndicatorHorizontalChart
            ? ThemeManager::GetInstance().GetCurrentTheme().GetPrimaryColor().ToLvOpa() : LV_OPA_COVER);
        ThemeManager::GetInstance().SetTheme(std::make_shared<DefaultTheme>());
        widget->OnDeactivated();
        widget->OnActivated();
        zassert_equal(lv_obj_get_style_opa_layered(outer, LV_PART_MAIN), 128);
        zassert_equal(lv_obj_get_child(outer, 0), inner);
        configuration->properties[WidgetPropertyType::OPACITY] = 0;
        widget->Configure(configuration);
        zassert_equal(lv_obj_get_style_opa_layered(outer, LV_PART_MAIN), 0);
        configuration->properties.erase(WidgetPropertyType::OPACITY);
        widget->Configure(configuration);
        zassert_equal(lv_obj_get_style_opa_layered(outer, LV_PART_MAIN), LV_OPA_COVER);
    }
}

ZTEST(widget_colors, test_opacity_binding_restores_retained_colors_after_all_gates_open) {
    auto configuration = Configuration(WidgetType::BasicIcon);
    configuration->properties[primary] = std::pmr::string("#12345660");
    auto widget = Render(configuration);
    auto* outer = widget->GetContainer()->GetObject();
    auto* inner = Inner(*widget);
    Publish(WidgetPropertyType::OPACITY, 0);
    Color(primary, "#ABCDEF80");
    Color(primary, "#FEDCBA40");
    CheckBackground(inner, LV_PART_MAIN, LvglColor(0x123456, 96));
    zassert_equal(lv_obj_get_style_opa_layered(outer, LV_PART_MAIN), 0);
    Publish(WidgetPropertyType::IS_VISIBLE, false);
    Publish(WidgetPropertyType::IS_ACTIVE, false);
    widget->OnDeactivated();
    Publish(WidgetPropertyType::OPACITY, 128);
    zassert_equal(lv_obj_get_style_opa_layered(outer, LV_PART_MAIN), 128);
    Publish(WidgetPropertyType::IS_VISIBLE, true);
    Publish(WidgetPropertyType::IS_ACTIVE, true);
    CheckBackground(inner, LV_PART_MAIN, LvglColor(0x123456, 96));
    widget->OnActivated();
    CheckBackground(inner, LV_PART_MAIN, LvglColor(0xFEDCBA, 64));
    zassert_equal(lv_obj_get_style_opa_layered(outer, LV_PART_MAIN), 128);
    for(const EventData& invalid : { EventData{-1}, EventData{256}, EventData{128.5F}, EventData{true} }) {
        Publish(WidgetPropertyType::OPACITY, invalid);
        zassert_equal(lv_obj_get_style_opa_layered(outer, LV_PART_MAIN), 128);
    }
    Publish(WidgetPropertyType::OPACITY, uint32_t{255});
    zassert_equal(lv_obj_get_style_opa_layered(outer, LV_PART_MAIN), LV_OPA_COVER);
}

ZTEST(widget_colors, test_value_animation_and_theme_changes_preserve_whole_widget_opacity) {
    auto configuration = Configuration(WidgetType::IndicatorBar);
    configuration->properties[WidgetPropertyType::OPACITY] = 128;
    configuration->properties[WidgetPropertyType::IS_SMOOTHED] = true;
    auto widget = Render(configuration);
    auto* outer = widget->GetContainer()->GetObject();
    auto* indicator = dynamic_cast<WidgetBase*>(widget.get());
    Publish(WidgetPropertyType::VALUE, 100.0F);
    zassert_not_null(lv_anim_get(indicator, nullptr));
    lv_tick_inc(100);
    lv_anim_refr_now();
    ThemeManager::GetInstance().SetTheme(std::make_shared<DefaultTheme>());
    zassert_equal(lv_obj_get_style_opa_layered(outer, LV_PART_MAIN), 128);
    Publish(WidgetPropertyType::OPACITY, 0);
    zassert_is_null(lv_anim_get(indicator, nullptr));
    Publish(WidgetPropertyType::VALUE, 75.0F);
    ThemeManager::GetInstance().SetTheme(std::make_shared<TranslucentTheme>());
    lv_tick_inc(100);
    lv_anim_refr_now();
    zassert_equal(lv_obj_get_style_opa_layered(outer, LV_PART_MAIN), 0);
    Publish(WidgetPropertyType::OPACITY, 128);
    zassert_not_null(lv_anim_get(indicator, nullptr));
    lv_tick_inc(5000);
    lv_anim_refr_now();
    zassert_equal(lv_bar_get_value(Inner(*widget)), 75);
    zassert_equal(lv_obj_get_style_opa_layered(outer, LV_PART_MAIN), 128);
}

ZTEST(widget_colors, test_text_colors_follow_theme_only_when_unset) {
    for(auto type : { WidgetType::IndicatorDigital, WidgetType::IndicatorSetting }) {
        auto configuration = Configuration(type);
        auto widget = Render(configuration);
        auto* label = Inner(*widget);
        const std::array<std::shared_ptr<ITheme>, 4> themes {
            std::make_shared<DefaultTheme>(), std::make_shared<DarkTheme>(),
            std::make_shared<DarkBWTheme>(), std::make_shared<TranslucentTheme>()
        };
        for(const auto& theme : themes) {
            ThemeManager::GetInstance().SetTheme(theme);
            CheckText(label, theme->GetPrimaryColor());
            Color(primary, "#fedcba80");
            CheckText(label, LvglColor(0xFEDCBA, 128));
            ThemeManager::GetInstance().SetTheme(std::make_shared<DefaultTheme>());
            CheckText(label, LvglColor(0xFEDCBA, 128));
            Color(primary, "");
            ThemeManager::GetInstance().SetTheme(theme);
            CheckText(label, theme->GetPrimaryColor());
        }
        zassert_equal(Inner(*widget), label);
        zassert_equal(configuration->properties.count(primary), 0);
    }
}

ZTEST(widget_colors, test_fill_and_track_resolve_independently_and_reset) {
    for(auto type : { WidgetType::IndicatorBar, WidgetType::IndicatorArcFill }) {
        auto widget = Render(Configuration(type));
        auto* object = Inner(*widget);
        auto check = type == WidgetType::IndicatorBar ? CheckBackground : CheckArc;
        const auto& theme = ThemeManager::GetInstance().GetCurrentTheme();
        check(object, LV_PART_INDICATOR, theme.GetSecondaryColor());
        check(object, LV_PART_MAIN, LvglColor(0, 0));
        Color(primary, "#ff220080");
        Color(secondary, "#0033ff40");
        check(object, LV_PART_INDICATOR, LvglColor(0xFF2200, 128));
        check(object, LV_PART_MAIN, LvglColor(0x0033FF, 64));
        Color(secondary, "");
        check(object, LV_PART_MAIN, LvglColor(0, 0));
        check(object, LV_PART_INDICATOR, LvglColor(0xFF2200, 128));
        Color(primary, "");
        check(object, LV_PART_INDICATOR, theme.GetSecondaryColor());
        zassert_equal(Inner(*widget), object);
    }
}

ZTEST(widget_colors, test_slider_palettes_do_not_inherit_other_states_or_roles) {
    auto widget = Render(Configuration(WidgetType::ControlSlider));
    auto* slider = Inner(*widget);
    const auto& theme = ThemeManager::GetInstance().GetCurrentTheme();
    Color(primary_inactive, "#ff000080");
    Color(secondary, "#00ff0040");
    Color(tertiary_inactive, "#0000ff20");
    CheckBackground(slider, LV_PART_INDICATOR, LvglColor(0xFF0000, 128));
    CheckBackground(slider, LV_PART_MAIN, theme.GetSurfaceColor());
    CheckBackground(slider, LV_PART_KNOB, LvglColor(0x0000FF, 32));
    lv_obj_add_state(slider, LV_STATE_PRESSED);
    zassert_equal(lv_obj_get_style_recolor_opa(slider, LV_PART_INDICATOR), LV_OPA_TRANSP);
    CheckBackground(slider, LV_PART_INDICATOR, theme.GetPrimaryColor());
    CheckBackground(slider, LV_PART_MAIN, LvglColor(0x00FF00, 64));
    CheckBackground(slider, LV_PART_KNOB, theme.GetAccentColor());
    Color(primary, "#11223300");
    Color(tertiary, "#aabbccff");
    CheckBackground(slider, LV_PART_INDICATOR, LvglColor(0x112233, 0));
    CheckBackground(slider, LV_PART_KNOB, LvglColor(0xAABBCC, 255));
    Color(secondary, "");
    CheckBackground(slider, LV_PART_MAIN, theme.GetSurfaceColor());
    lv_obj_remove_state(slider, LV_STATE_PRESSED);
    CheckBackground(slider, LV_PART_INDICATOR, LvglColor(0xFF0000, 128));
    zassert_equal(lv_slider_get_value(slider), 0);
}

ZTEST(widget_colors, test_toggle_track_and_knob_select_checked_colors) {
    auto widget = Render(Configuration(WidgetType::ControlToggle));
    auto* toggle = Inner(*widget);
    const auto& theme = ThemeManager::GetInstance().GetCurrentTheme();
    Color(primary_inactive, "#ff000040");
    Color(primary, "#00ff0080");
    Color(secondary_inactive, "#0000ff20");
    CheckBackground(toggle, LV_PART_MAIN, LvglColor(0xFF0000, 64));
    CheckBackground(toggle, LV_PART_KNOB, LvglColor(0x0000FF, 32));
    Publish(WidgetPropertyType::VALUE, true);
    zassert_true(lv_obj_has_state(toggle, LV_STATE_CHECKED));
    CheckBackground(toggle, LV_PART_MAIN, LvglColor(0xFF0000, 64));
    CheckBackground(toggle, LV_PART_INDICATOR, LvglColor(0x00FF00, 128));
    CheckBackground(toggle, LV_PART_KNOB, theme.GetAccentColor());
    Color(secondary, "#112233ff");
    CheckBackground(toggle, LV_PART_KNOB, LvglColor(0x112233, 255));
    Color(primary, "");
    CheckBackground(toggle, LV_PART_INDICATOR, theme.GetPrimaryColor());
    Publish(WidgetPropertyType::VALUE, false);
    CheckBackground(toggle, LV_PART_KNOB, LvglColor(0x0000FF, 32));
    zassert_equal(lv_obj_get_style_bg_opa(toggle, LV_PART_INDICATOR), LV_OPA_TRANSP);
}

ZTEST(widget_colors, test_button_label_tracks_press_release_and_cancel) {
    auto configuration = Configuration(WidgetType::ControlButton);
    configuration->properties[WidgetPropertyType::LABEL] = std::pmr::string("Color");
    auto widget = Render(configuration);
    auto* button = Inner(*widget);
    auto* label = lv_obj_get_child(button, 0);
    const auto& theme = ThemeManager::GetInstance().GetCurrentTheme();
    Color(primary_inactive, "#ff000040");
    Color(secondary_inactive, "#00ff0080");
    CheckBackground(button, LV_PART_MAIN, LvglColor(0xFF0000, 64));
    CheckText(label, LvglColor(0x00FF00, 128));
    for(auto release : { LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST }) {
        lv_obj_add_state(button, LV_STATE_PRESSED);
        lv_obj_send_event(button, LV_EVENT_PRESSED, nullptr);
        zassert_true(lv_obj_has_state(label, LV_STATE_PRESSED));
        zassert_equal(lv_obj_get_style_recolor_opa(button, LV_PART_MAIN), LV_OPA_TRANSP);
        CheckBackground(button, LV_PART_MAIN, theme.GetAccentColor());
        CheckText(label, theme.GetPrimaryColor());
        Color(primary, "#aabbccff");
        Color(secondary, "#11223300");
        CheckBackground(button, LV_PART_MAIN, LvglColor(0xAABBCC, 255));
        CheckText(label, LvglColor(0x112233, 0));
        Color(primary, "");
        Color(secondary, "");
        CheckText(label, theme.GetPrimaryColor());
        lv_obj_remove_state(button, LV_STATE_PRESSED);
        lv_obj_send_event(button, release, nullptr);
        zassert_false(lv_obj_has_state(label, LV_STATE_PRESSED));
        CheckText(label, LvglColor(0x00FF00, 128));
    }
    lv_obj_add_state(button, LV_STATE_PRESSED);
    lv_obj_send_event(button, LV_EVENT_PRESSED, nullptr);
    Publish(WidgetPropertyType::IS_ACTIVE, false);
    zassert_false(lv_obj_has_state(label, LV_STATE_PRESSED));
}

ZTEST(widget_colors, test_dot_label_and_shapes_apply_colors_without_rebuilding) {
    for(auto type : { IconType::Dot, IconType::Label, IconType::Rectangle, IconType::TriangleIsosceles,
                     IconType::TriangleRight, IconType::Oval, IconType::Line }) {
        auto configuration = Configuration(WidgetType::BasicIcon, type);
        configuration->properties[WidgetPropertyType::LABEL] = std::pmr::string("Log");
        auto widget = Render(configuration);
        auto* object = Inner(*widget);
        const bool shape = type != IconType::Dot && type != IconType::Label;
        const auto* source = shape ? lv_image_get_src(object) : nullptr;
        Color(primary, "#11223380");
        if(shape) {
            CheckColor(lv_obj_get_style_image_recolor(object, LV_PART_MAIN), LvglColor(0x112233));
            zassert_equal(lv_obj_get_style_opa(object, LV_PART_MAIN), 128);
            zassert_equal(lv_image_get_src(object), source);
        } else {
            CheckBackground(object, LV_PART_MAIN, LvglColor(0x112233, 128));
            zassert_equal(lv_obj_get_style_opa(object, LV_PART_MAIN), LV_OPA_COVER);
        }
        if(type == IconType::Label) {
            auto* label = lv_obj_get_child(object, 0);
            CheckText(label, ThemeManager::GetInstance().GetCurrentTheme().GetPrimaryColor());
            Color(secondary, "#aabbcc40");
            CheckText(label, LvglColor(0xAABBCC, 64));
        }
        Publish(WidgetPropertyType::IS_ACTIVE, false);
        Color(primary, "#ffffffff");
        if(!shape)
            CheckBackground(object, LV_PART_MAIN, LvglColor(0x112233, 128));
        zassert_equal(Inner(*widget), object);
    }
}

ZTEST(widget_colors, test_direct_icon_configuration_applies_parsed_colors) {
    auto root = std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(100, false).SetHeight(100, false).Build());
    auto properties = std::make_shared<WidgetPropertyStore>();
    auto& factory = eerie_leap::views::widgets::basic::icons::IconFactory::GetInstance();
    factory.RegisterProperties(IconType::Dot, *properties);
    zassert_true(properties->Set(primary, std::pmr::string("#11223380")));
    auto icon = factory.Create(IconType::Dot, properties, root);
    zassert_equal(icon->Render(), 0);
    CheckBackground(icon->GetContainer()->GetObject(), LV_PART_MAIN, LvglColor(0x112233, 128));
    zassert_true(properties->Set(primary, std::pmr::string("")));
    icon->Configure(properties);
    icon->ApplyTheme(ThemeManager::GetInstance().GetCurrentTheme());
    CheckBackground(icon->GetContainer()->GetObject(), LV_PART_MAIN,
        ThemeManager::GetInstance().GetCurrentTheme().GetAccentColor());
}

ZTEST(widget_colors, test_chart_color_changes_preserve_series_and_samples) {
    using eerie_leap::views::widgets::indicators::HorizontalChartIndicatorType;
    for(auto chart_type : { HorizontalChartIndicatorType::Bar, HorizontalChartIndicatorType::Line }) {
        auto configuration = Configuration(WidgetType::IndicatorHorizontalChart);
        configuration->properties[WidgetPropertyType::CHART_TYPE] = static_cast<int>(chart_type);
        auto widget = Render(configuration);
        auto* chart = Inner(*widget);
        Publish(WidgetPropertyType::VALUE, 40);
        auto* series = lv_chart_get_series_next(chart, nullptr);
        auto* samples = lv_chart_get_y_array(chart, series);
        const auto count = lv_chart_get_point_count(chart);
        const std::vector<int32_t> before(samples, samples + count);
        Color(primary, "#11223380");
        zassert_equal(lv_obj_get_style_opa_layered(chart, LV_PART_MAIN), 128);
        zassert_equal(lv_obj_get_style_line_opa(chart, LV_PART_ITEMS), LV_OPA_COVER);
        zassert_equal(lv_obj_get_style_bg_opa(chart, LV_PART_ITEMS), LV_OPA_COVER);
        zassert_equal(lv_chart_get_series_next(chart, nullptr), series);
        zassert_true(std::equal(before.begin(), before.end(), samples));
        Color(primary, "");
        zassert_equal(lv_obj_get_style_opa_layered(chart, LV_PART_MAIN),
            ThemeManager::GetInstance().GetCurrentTheme().GetPrimaryColor().ToLvOpa());
        zassert_true(std::equal(before.begin(), before.end(), samples));
    }
}

ZTEST(widget_colors, test_segment_palettes_preserve_displayed_states) {
    auto widget = Render(Configuration(WidgetType::IndicatorSegmentArc));
    auto* container = widget->GetContainer()->GetObject();
    auto* first = lv_obj_get_child(container, 0);
    auto* last = lv_obj_get_child(container, -1);
    Publish(WidgetPropertyType::VALUE, 20);
    Color(primary, "#11223380");
    Color(primary_inactive, "#aabbcc40");
    CheckArc(first, LV_PART_INDICATOR, LvglColor(0x112233, 128));
    CheckArc(last, LV_PART_INDICATOR, LvglColor(0xAABBCC, 64));
    ThemeManager::GetInstance().SetTheme(std::make_shared<DarkTheme>());
    CheckArc(first, LV_PART_INDICATOR, LvglColor(0x112233, 128));
    CheckArc(last, LV_PART_INDICATOR, LvglColor(0xAABBCC, 64));
    Publish(WidgetPropertyType::VALUE, 100);
    CheckArc(last, LV_PART_INDICATOR, LvglColor(0x112233, 128));
    Publish(WidgetPropertyType::VALUE, 20);
    Color(primary_inactive, "");
    CheckArc(last, LV_PART_INDICATOR, LvglColor(0, 0));
    Color(primary, "");
    CheckArc(first, LV_PART_INDICATOR, ThemeManager::GetInstance().GetCurrentTheme().GetSecondaryColor());
    CheckArc(last, LV_PART_INDICATOR, LvglColor(0, 0));
}

ZTEST(widget_colors, test_hidden_colors_replay_latest_and_inactive_colors_are_rejected) {
    auto configuration = Configuration(WidgetType::BasicIcon);
    configuration->properties[primary] = std::pmr::string("#112233ff");
    auto widget = Render(configuration);
    auto* dot = Inner(*widget);
    for(auto gate : { WidgetPropertyType::IS_VISIBLE, WidgetPropertyType::OPACITY }) {
        Publish(gate, 0);
        Color(primary, "#ff000080");
        Color(primary, "#00ff0040");
        CheckBackground(dot, LV_PART_MAIN, LvglColor(0x112233, 255));
        ThemeManager::GetInstance().SetTheme(std::make_shared<DarkTheme>());
        CheckBackground(dot, LV_PART_MAIN, LvglColor(0x112233, 255));
        Publish(gate, 1);
        CheckBackground(dot, LV_PART_MAIN, LvglColor(0x00FF00, 64));
        Color(primary, "#112233ff");
    }
    Publish(WidgetPropertyType::IS_ACTIVE, false);
    Color(primary, "#ffffffff");
    Publish(WidgetPropertyType::IS_ACTIVE, true);
    CheckBackground(dot, LV_PART_MAIN, LvglColor(0x112233, 255));
    widget->Configure(Configuration(WidgetType::BasicIcon));
    CheckBackground(dot, LV_PART_MAIN, ThemeManager::GetInstance().GetCurrentTheme().GetAccentColor());
}
