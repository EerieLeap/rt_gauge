#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/assets/assets_manager.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "domain/ui_domain/models/animation.h"
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
    return lv_obj_get_child(views_test::WidgetContent(widget), 0);
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

void GallerySnapshot(lv_obj_t* root, const char* name, bool visible = true) {
    lv_obj_update_layout(root);
    std::unique_ptr<lv_draw_buf_t, decltype(&lv_draw_buf_destroy)> snapshot(
        lv_snapshot_take(root, LV_COLOR_FORMAT_ARGB8888), lv_draw_buf_destroy);
    zassert_not_null(snapshot);
    zassert_equal(snapshot->header.w, 96);
    zassert_equal(snapshot->header.h, 72);
    const auto* background = reinterpret_cast<const lv_color32_t*>(snapshot->data);
    size_t changed = 0;
    for(uint32_t row = 0; row < snapshot->header.h; ++row) {
        auto* pixels = reinterpret_cast<const lv_color32_t*>(snapshot->data + row * snapshot->header.stride);
        for(uint32_t column = 0; column < snapshot->header.w; ++column) {
            if(pixels[column].red != background->red || pixels[column].green != background->green
                || pixels[column].blue != background->blue)
                ++changed;
        }
    }
    zassert_equal(changed > 0, visible, "%s visibility", name);
#ifdef CONFIG_ARCH_POSIX
    const char* directory = std::getenv("WIDGET_COLOR_PREVIEW_DIR");
    if(directory == nullptr)
        return;
    char path[512];
    const int length = std::snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    zassert_true(length > 0 && static_cast<size_t>(length) < sizeof(path));
    auto* file = std::fopen(path, "wb");
    zassert_not_null(file, "%s", path);
    std::fprintf(file, "P6\n%u %u\n255\n", snapshot->header.w, snapshot->header.h);
    for(uint32_t row = 0; row < snapshot->header.h; ++row) {
        auto* pixels = reinterpret_cast<const lv_color32_t*>(snapshot->data + row * snapshot->header.stride);
        for(uint32_t column = 0; column < snapshot->header.w; ++column) {
            const uint8_t rgb[] { pixels[column].red, pixels[column].green, pixels[column].blue };
            zassert_equal(std::fwrite(rgb, 1, sizeof(rgb), file), sizeof(rgb));
        }
    }
    zassert_equal(std::fclose(file), 0);
#endif
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

ZTEST(widget_colors, test_animation_simulator_gallery) {
    eerie_leap::domain::ui_domain::ScopedLvglLock lock;
    ThemeManager::GetInstance().SetTheme(std::make_shared<DefaultTheme>());
    const auto baseline = lv_anim_count_running();
    const std::array types { WidgetType::BasicIcon, WidgetType::BasicIcon,
        WidgetType::IndicatorDial, WidgetType::ControlButton };
    const char* names[][3] {
        { "animation_dot_full", "animation_dot_half", "animation_dot_zero" },
        { "animation_label_0", "animation_label_45", "animation_label_90" },
        { "animation_dial_0", "animation_dial_45", "animation_dial_90" },
        { "animation_button_0", "animation_button_45", "animation_button_90" }
    };
    for(size_t index = 0; index < types.size(); ++index) {
        auto root = std::make_shared<Frame>(
            Frame::CreateWrapped().SetWidth(96, true).SetHeight(72, true).Build());
        lv_obj_set_style_bg_color(root->GetObject(), lv_color_hex(0x202428), 0);
        lv_obj_set_style_bg_opa(root->GetObject(), LV_OPA_COVER, 0);
        auto configuration = Configuration(types[index], index == 0 ? IconType::Dot : IconType::Label);
        configuration->bindings.clear();
        configuration->properties[WidgetPropertyType::LABEL] = std::pmr::string(index == 3 ? "LOG" : "RPM");
        configuration->properties[WidgetPropertyType::IS_SMOOTHED] = false;
        configuration->properties[WidgetPropertyType::ANIMATION_TYPE] = static_cast<int>(
            index == 0 ? Animation::Type::Blinking : Animation::Type::Rotation);
        configuration->properties[WidgetPropertyType::IS_ANIMATION_ACTIVE] = true;
        configuration->properties[WidgetPropertyType::ANIMATION_DURATION_MS] = 1000;
        WidgetContext context;
        if(index == 2) {
            using eerie_leap::subsys::device_tree::DtFs;
            using eerie_leap::subsys::fs::services::FsService;
            DtFs::InitInternalFs();
            auto fs = std::make_shared<FsService>(DtFs::GetInternalFsMp());
            zassert_true(fs->Initialize());
            context.assets_manager = std::make_shared<AssetsManager>(fs, "animation-gallery");
            std::array<uint8_t, 8 * 28 * 4> pixels;
            pixels.fill(255);
            zassert_true(context.assets_manager->Save("needle.bin", pixels));
            configuration->properties[WidgetPropertyType::FILE_PATH] = std::pmr::string("needle.bin");
            configuration->properties[WidgetPropertyType::IMG_WIDTH] = 8;
            configuration->properties[WidgetPropertyType::IMG_HEIGHT] = 28;
            configuration->properties[WidgetPropertyType::START_ANGLE] = 180;
            configuration->properties[WidgetPropertyType::END_ANGLE] = 450;
            configuration->properties[WidgetPropertyType::VALUE] = 25;
        }
        auto widget = WidgetFactory::GetInstance().CreateWidget(configuration, root, context);
        widget->SetSizePx({ index == 2 ? 48 : 64, index == 2 ? 48 : 24 });
        widget->SetPositionPx({ index == 2 ? 24 : 16, index == 2 ? 12 : 24 });
        zassert_equal(widget->Render(), 0);
        widget->OnActivated();
        zassert_equal(lv_anim_count_running(), baseline + 1);
        auto* content = views_test::WidgetContent(*widget);
        for(int phase = 0; phase < 3; ++phase) {
            if(phase != 0) {
                lv_tick_inc(index == 0 ? 250 : 125);
                lv_anim_refr_now();
            }
            if(index == 0)
                zassert_within(lv_obj_get_style_opa_layered(content, LV_PART_MAIN),
                    phase == 0 ? 255 : phase == 1 ? 128 : 0, 2);
            else
                zassert_equal(lv_obj_get_style_transform_rotation(content, LV_PART_MAIN), phase * 450);
            GallerySnapshot(root->GetObject(), names[index][phase], index != 0 || phase != 2);
            zassert_true(static_cast<WidgetBase*>(widget.get())->IsProcessingEligible());
        }
        widget.reset();
        zassert_equal(lv_anim_count_running(), baseline);
    }
}

ZTEST(widget_colors, test_simulator_gallery) {
    auto root = std::make_shared<Frame>(
        Frame::CreateWrapped().SetWidth(96, true).SetHeight(72, true).Build());
    auto parent = std::make_shared<Frame>(
        Frame::CreateWrapped(root->GetObject()).SetWidth(72, true).SetHeight(32, true).Build());
    lv_obj_center(parent->GetObject());
    lv_obj_set_style_bg_color(root->GetObject(), lv_color_hex(0x202428), 0);
    lv_obj_set_style_bg_opa(root->GetObject(), LV_OPA_COVER, 0);
    auto render = [&](std::shared_ptr<WidgetConfiguration> configuration) {
        configuration->properties[WidgetPropertyType::IS_SMOOTHED] = false;
        auto widget = WidgetFactory::GetInstance().CreateWidget(configuration, parent, WidgetContext{});
        zassert_equal(widget->Render(), 0);
        widget->OnActivated();
        lv_obj_update_layout(root->GetObject());
        return widget;
    };

    const std::array<std::shared_ptr<ITheme>, 4> themes {
        std::make_shared<DefaultTheme>(), std::make_shared<DarkTheme>(),
        std::make_shared<DarkBWTheme>(), std::make_shared<TranslucentTheme>()
    };
    const char* theme_names[] { "01_default", "02_dark", "03_monochrome", "04_theme_alpha" };
    auto configuration = Configuration(WidgetType::IndicatorBar);
    configuration->bindings.clear();
    configuration->properties[WidgetPropertyType::VALUE] = 70;
    {
        auto widget = render(configuration);
        for(size_t index = 0; index < themes.size(); ++index) {
            ThemeManager::GetInstance().SetTheme(themes[index]);
            CheckBackground(Inner(*widget), LV_PART_INDICATOR, themes[index]->GetSecondaryColor());
            GallerySnapshot(root->GetObject(), theme_names[index]);
        }
        configuration->properties[primary] = std::pmr::string("#33CC9980");
        configuration->properties[secondary] = std::pmr::string("#6688AA40");
        widget->Configure(configuration);
        CheckBackground(Inner(*widget), LV_PART_INDICATOR, LvglColor(0x33CC99, 128));
        GallerySnapshot(root->GetObject(), "05_explicit_rgba");
        configuration->properties[WidgetPropertyType::OPACITY] = 128;
        widget->Configure(configuration);
        zassert_equal(lv_obj_get_style_opa_layered(widget->GetContainer()->GetObject(), LV_PART_MAIN), 128);
        GallerySnapshot(root->GetObject(), "06_widget_fade");
    }

    ThemeManager::GetInstance().SetTheme(std::make_shared<DefaultTheme>());
    for(auto type : { WidgetType::ControlButton, WidgetType::ControlSlider, WidgetType::ControlToggle }) {
        configuration = Configuration(type);
        configuration->bindings.clear();
        configuration->properties[WidgetPropertyType::LABEL] = std::pmr::string("LOG");
        configuration->properties[primary] = std::pmr::string("#33CC99FF");
        configuration->properties[primary_inactive] = std::pmr::string("#6688AA80");
        configuration->properties[secondary] = std::pmr::string("#FFFFFFFF");
        configuration->properties[secondary_inactive] = std::pmr::string("#DDEEFFC0");
        if(type == WidgetType::ControlSlider) {
            configuration->properties[secondary] = std::pmr::string("#6688AA80");
            configuration->properties[secondary_inactive] = std::pmr::string("#6688AA40");
            configuration->properties[tertiary] = std::pmr::string("#FFFFFFFF");
            configuration->properties[tertiary_inactive] = std::pmr::string("#DDEEFFC0");
            configuration->properties[WidgetPropertyType::MIN_VALUE] = 0;
            configuration->properties[WidgetPropertyType::MAX_VALUE] = 100;
            configuration->properties[WidgetPropertyType::STEP] = 1;
            configuration->properties[WidgetPropertyType::VALUE] = 65;
        }
        auto widget = render(configuration);
        auto* object = Inner(*widget);
        const bool toggle = type == WidgetType::ControlToggle;
        const bool button = type == WidgetType::ControlButton;
        if(type == WidgetType::ControlSlider)
            zassert_equal(lv_slider_get_value(object), 65);
        GallerySnapshot(root->GetObject(), button ? "07_button_idle" : toggle ? "11_toggle_off" : "09_slider_idle");
        if(toggle) {
            configuration->properties[WidgetPropertyType::VALUE] = true;
            widget->Configure(configuration);
            zassert_true(lv_obj_has_state(object, LV_STATE_CHECKED));
        } else {
            lv_obj_add_state(object, LV_STATE_PRESSED);
            lv_obj_send_event(object, LV_EVENT_PRESSED, nullptr);
            zassert_true(lv_obj_has_state(object, LV_STATE_PRESSED));
        }
        lv_tick_inc(500);
        lv_anim_refr_now();
        if(type == WidgetType::ControlSlider)
            zassert_equal(lv_slider_get_value(object), 65);
        GallerySnapshot(root->GetObject(), button ? "08_button_pressed" : toggle ? "12_toggle_on" : "10_slider_pressed");
    }

    configuration = Configuration(WidgetType::IndicatorBar);
    std::erase_if(configuration->bindings, [](const auto& binding) {
        return binding.target != WidgetPropertyType::VALUE && binding.target != WidgetPropertyType::IS_VISIBLE
            && binding.target != WidgetPropertyType::IS_ACTIVE && binding.target != WidgetPropertyType::OPACITY;
    });
    configuration->properties[primary] = std::pmr::string("#33CC99FF");
    configuration->properties[secondary] = std::pmr::string("#6688AA40");
    auto widget = render(configuration);
    auto* bar = Inner(*widget);
    Publish(WidgetPropertyType::VALUE, 25);
    zassert_equal(lv_bar_get_value(bar), 25);
    GallerySnapshot(root->GetObject(), "13_initial_25");
    Publish(WidgetPropertyType::IS_VISIBLE, false);
    Publish(WidgetPropertyType::VALUE, 50);
    Publish(WidgetPropertyType::VALUE, 80);
    zassert_equal(lv_bar_get_value(bar), 25);
    GallerySnapshot(root->GetObject(), "14_hidden_80", false);
    Publish(WidgetPropertyType::IS_VISIBLE, true);
    zassert_equal(lv_bar_get_value(bar), 80);
    GallerySnapshot(root->GetObject(), "15_revealed_80");
    Publish(WidgetPropertyType::OPACITY, 0);
    Publish(WidgetPropertyType::VALUE, 60);
    zassert_equal(lv_bar_get_value(bar), 80);
    GallerySnapshot(root->GetObject(), "16_transparent_60", false);
    Publish(WidgetPropertyType::OPACITY, 255);
    zassert_equal(lv_bar_get_value(bar), 60);
    GallerySnapshot(root->GetObject(), "17_restored_60");
    Publish(WidgetPropertyType::IS_ACTIVE, false);
    Publish(WidgetPropertyType::VALUE, 95);
    Publish(WidgetPropertyType::IS_ACTIVE, true);
    zassert_equal(lv_bar_get_value(bar), 60);
    GallerySnapshot(root->GetObject(), "18_rejected_95");
}

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
        auto* inner = lv_obj_get_child(views_test::WidgetContent(*widget), 0);
        zassert_equal(lv_obj_get_style_opa_layered(outer, LV_PART_MAIN), 128);
        zassert_equal(lv_obj_get_style_opa_layered(inner, LV_PART_MAIN), type == WidgetType::IndicatorHorizontalChart
            ? ThemeManager::GetInstance().GetCurrentTheme().GetPrimaryColor().ToLvOpa() : LV_OPA_COVER);
        ThemeManager::GetInstance().SetTheme(std::make_shared<DefaultTheme>());
        widget->OnDeactivated();
        widget->OnActivated();
        zassert_equal(lv_obj_get_style_opa_layered(outer, LV_PART_MAIN), 128);
        zassert_equal(lv_obj_get_child(views_test::WidgetContent(*widget), 0), inner);
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
    auto* container = views_test::WidgetContent(*widget);
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
