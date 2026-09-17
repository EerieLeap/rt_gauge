#include <array>
#include <memory>
#include <utility>

#include <zephyr/ztest.h>

#include "subsys/assets/assets_manager.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "domain/logging_domain/event_bus/logging_events_channel.h"
#include "domain/sensor_domain/event_bus/sensor_events_channel.h"
#include "event_bus/event_channel_id.h"
#include "event_bus/event_channels.h"
#include "views/themes/default_theme.h"
#include "views/widgets/basic/icon_widget/icon_widget.h"
#include "views/widgets/basic/icons/dot_icon/dot_icon.h"
#include "views/widgets/indicators/dial_indicator/dial_indicator.h"

#include "views_test_support.h"

using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::widgets;
using namespace eerie_leap::views::widgets::basic;
using namespace eerie_leap::views::themes;
using namespace eerie_leap::domain::sensor_domain::event_bus;
using namespace eerie_leap::domain::logging_domain::event_bus;
using eerie_leap::event_bus::EventChannelId;
using eerie_leap::event_bus::InitializeEventChannels;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::views::utilitites::Frame;

namespace {

class TestIcon : public IconWidget {
public:
    using IconWidget::IconWidget;
    icons::DotIcon* Dot() const { return dynamic_cast<icons::DotIcon*>(icon_.get()); }
    ConfigValue Read(WidgetPropertyType type) const { return properties_->Get(type); }
};

class TestDial : public indicators::DialIndicator {
public:
    using DialIndicator::DialIndicator;
    WidgetBase& Needle() const { return *dependencies_.front(); }
};

class BlueTheme : public DefaultTheme {
    LvglColor GetAccentColor() const override { return LvglColor(0x3366FF, 128); }
};

std::shared_ptr<Frame> MakeRoot() {
    return std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(100, true).SetHeight(100, true).Build());
}

std::shared_ptr<WidgetConfiguration> Configuration(IconType type) {
    auto configuration = std::make_shared<WidgetConfiguration>(std::allocator_arg, std::pmr::get_default_resource());
    configuration->properties[WidgetPropertyType::ICON_TYPE] = static_cast<int>(type);
    for(auto target : { WidgetPropertyType::IS_ACTIVE, WidgetPropertyType::IS_VISIBLE,
                       WidgetPropertyType::OPACITY, WidgetPropertyType::POSITION_X, WidgetPropertyType::VALUE }) {
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

void PublishLogging(bool active) {
    LoggingEventsChannel::GetInstance().Publish({
        .source_id = 0,
        .type = LoggingEventType::StatusUpdated,
        .payload = {{ LoggingPayloadType::IsActive, active }}
    });
}

lv_obj_t* Inner(const IconWidget& widget) {
    return widget.GetContainer()->GetChild()->GetObject();
}

lv_anim_t* Pulse(const TestIcon& widget) {
    return lv_anim_get(widget.Dot(), nullptr);
}

void Tick(uint32_t elapsed) {
    lv_tick_inc(elapsed);
    lv_anim_refr_now();
}

WidgetContext ImageContext(WidgetConfiguration& configuration) {
    using eerie_leap::subsys::device_tree::DtFs;
    using eerie_leap::subsys::fs::services::FsService;
    auto fs = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    zassert_true(fs->Initialize());
    auto assets = std::make_shared<AssetsManager>(fs, "icon-lifecycle");
    std::array<uint8_t, 64> pixels;
    pixels.fill(255);
    zassert_true(assets->Save("needle.bin", pixels));
    configuration.properties[WidgetPropertyType::FILE_PATH] = std::pmr::string("needle.bin");
    configuration.properties[WidgetPropertyType::IMG_WIDTH] = 4;
    configuration.properties[WidgetPropertyType::IMG_HEIGHT] = 4;
    return WidgetContext { .assets_manager = std::move(assets) };
}

void* Setup() {
    views_test::EnsureTestDisplay();
    InitializeEventChannels();
    return nullptr;
}

void Clean(void* fixture) {
    ThemeManager::GetInstance().SetTheme(std::make_shared<DefaultTheme>());
    views_test::CleanTestDisplay(fixture);
}

} // namespace

ZTEST_SUITE(icon_lifecycle, NULL, Setup, NULL, Clean, NULL);

ZTEST(icon_lifecycle, test_dot_management_restoration_does_not_start_animation) {
    auto configuration = Configuration(IconType::Dot);
    configuration->properties[WidgetPropertyType::IS_ACTIVE] = false;
    configuration->properties[WidgetPropertyType::IS_VISIBLE] = false;
    configuration->properties[WidgetPropertyType::OPACITY] = 0;
    TestIcon widget(1, MakeRoot(), WidgetContext{});
    widget.Configure(configuration);
    zassert_equal(widget.Render(), 0);
    widget.OnActivated();
    zassert_is_null(Pulse(widget));
    zassert_false(widget.IsVisible());
    zassert_true(lv_obj_has_flag(widget.GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN));
    ThemeManager::GetInstance().SetTheme(std::make_shared<BlueTheme>());
    Publish(WidgetPropertyType::IS_ACTIVE, true);
    Publish(WidgetPropertyType::IS_VISIBLE, true);
    zassert_is_null(Pulse(widget));
    zassert_true(widget.IsVisible());
    zassert_false(lv_obj_has_flag(widget.GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN));
    zassert_false(widget.IsProcessingEligible());
    Publish(WidgetPropertyType::OPACITY, 128);
    zassert_is_null(Pulse(widget));
    zassert_equal(lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN), LV_OPA_COVER);
    zassert_true(widget.IsProcessingEligible());
    Tick(200);
    zassert_equal(lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN), LV_OPA_COVER);
}

ZTEST(icon_lifecycle, test_dot_management_changes_keep_static_appearance) {
    for(auto target : { WidgetPropertyType::IS_ACTIVE, WidgetPropertyType::IS_VISIBLE, WidgetPropertyType::OPACITY }) {
        TestIcon widget(1, MakeRoot(), WidgetContext{});
        widget.Configure(Configuration(IconType::Dot));
        zassert_equal(widget.Render(), 0);
        zassert_is_null(Pulse(widget));
        widget.OnActivated();
        Tick(200);
        zassert_is_null(Pulse(widget));
        const auto opacity = lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN);
        zassert_equal(opacity, LV_OPA_COVER);
        Publish(target, 0);
        zassert_is_null(Pulse(widget));
        zassert_false(widget.IsProcessingEligible());
        zassert_equal(lv_obj_has_flag(widget.GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN),
            target == WidgetPropertyType::IS_VISIBLE);
        Tick(2000);
        zassert_equal(lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN), opacity);
        ThemeManager::GetInstance().SetTheme(std::make_shared<BlueTheme>());
        zassert_is_null(Pulse(widget));
        zassert_equal(lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN), opacity);
        zassert_equal(lv_obj_get_style_bg_opa(Inner(widget), LV_PART_MAIN), 128);
        Publish(target, 1);
        zassert_is_null(Pulse(widget));
        zassert_true(widget.IsProcessingEligible());
        zassert_false(lv_obj_has_flag(widget.GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN));
        zassert_equal(lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN), opacity);
        Tick(100);
        zassert_equal(lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN), opacity);
    }
}

ZTEST(icon_lifecycle, test_dot_ancestor_suspension_keeps_static_appearance) {
    for(bool hidden : { false, true }) {
        auto root = MakeRoot();
        TestIcon widget(1, root, WidgetContext{});
        widget.Configure(Configuration(IconType::Dot));
        zassert_equal(widget.Render(), 0);
        widget.OnActivated();
        Tick(200);
        const auto opacity = lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN);
        zassert_equal(opacity, LV_OPA_COVER);
        if(hidden)
            lv_obj_add_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_set_style_opa(root->GetObject(), 0, 0);
        Tick(50);
        zassert_false(widget.IsProcessingEligible());
        zassert_is_null(Pulse(widget));
        zassert_equal(lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN), opacity);
        lv_obj_remove_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(root->GetObject(), 255, 0);
        lv_refr_now(nullptr);
        zassert_true(widget.IsProcessingEligible());
        zassert_is_null(Pulse(widget));
        Tick(100);
        zassert_equal(lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN), opacity);
    }
}

ZTEST(icon_lifecycle, test_dot_group_lifecycle_and_teardown_do_not_start_animation) {
    const auto callbacks = lv_display_get_event_count(lv_display_get_default());
    icons::DotIcon* target = nullptr;
    for(bool suspended : { false, true }) {
        {
            TestIcon widget(1, MakeRoot(), WidgetContext{});
            widget.Configure(Configuration(IconType::Dot));
            zassert_equal(widget.Render(), 0);
            widget.OnActivated();
            target = widget.Dot();
            zassert_is_null(Pulse(widget));
            widget.OnDeactivated();
            zassert_is_null(Pulse(widget));
            zassert_false(widget.IsProcessingEligible());
            widget.OnActivated();
            zassert_is_null(Pulse(widget));
            zassert_true(widget.IsProcessingEligible());
            if(suspended)
                widget.OnDeactivated();
        }
        zassert_is_null(lv_anim_get(target, nullptr));
        zassert_equal(lv_display_get_event_count(lv_display_get_default()), callbacks);
        Tick(1000);
        lv_refr_now(nullptr);
    }
}

ZTEST(icon_lifecycle, test_logging_visibility_keeps_tracking_and_does_not_override_activity) {
    auto configuration = Configuration(IconType::Dot);
    configuration->properties[WidgetPropertyType::IS_VISIBLE] = false;
    configuration->bindings.push_back(PropertyBinding {
        .target = WidgetPropertyType::IS_VISIBLE,
        .channel = EventChannelId::Logging,
        .event_type = std::to_underlying(LoggingEventType::StatusUpdated),
        .payload_key = std::to_underlying(LoggingPayloadType::IsActive)
    });
    TestIcon widget(1, MakeRoot(), WidgetContext{});
    widget.Configure(configuration);
    zassert_equal(widget.Render(), 0);
    widget.OnActivated();
    zassert_true(widget.IsActive());
    zassert_true(lv_obj_has_flag(widget.GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN));
    zassert_is_null(Pulse(widget));
    Publish(WidgetPropertyType::POSITION_X, 12);
    zassert_equal(std::get<int>(widget.Read(WidgetPropertyType::POSITION_X)), 12);
    PublishLogging(true);
    zassert_true(widget.IsVisible());
    zassert_false(lv_obj_has_flag(widget.GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN));
    zassert_is_null(Pulse(widget));
    zassert_equal(lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN), LV_OPA_COVER);
    zassert_equal(lv_obj_get_style_x(Inner(widget), LV_PART_MAIN), 12);
    PublishLogging(false);
    zassert_false(widget.IsVisible());
    zassert_true(lv_obj_has_flag(widget.GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN));
    zassert_is_null(Pulse(widget));
    Publish(WidgetPropertyType::IS_ACTIVE, false);
    PublishLogging(true);
    zassert_true(widget.IsVisible());
    zassert_false(widget.IsActive());
    zassert_false(lv_obj_has_flag(widget.GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN));
    zassert_is_null(Pulse(widget));
    Publish(WidgetPropertyType::IS_ACTIVE, true);
    zassert_is_null(Pulse(widget));
    Tick(2000);
    zassert_equal(lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN), LV_OPA_COVER);
}

ZTEST(icon_lifecycle, test_inactive_label_keeps_the_normal_palette) {
    auto configuration = Configuration(IconType::Label);
    configuration->properties[WidgetPropertyType::LABEL] = std::pmr::string("log");
    configuration->properties[WidgetPropertyType::IS_ACTIVE] = false;
    TestIcon widget(1, MakeRoot(), WidgetContext{});
    widget.Configure(configuration);
    zassert_equal(widget.Render(), 0);
    widget.OnActivated();
    ThemeManager::GetInstance().SetTheme(std::make_shared<BlueTheme>());
    auto* object = Inner(widget);
    zassert_equal(lv_color_to_u32(lv_obj_get_style_bg_color(object, LV_PART_MAIN)), lv_color_to_u32(lv_color_hex(0x3366FF)));
    zassert_equal(lv_obj_get_style_bg_opa(object, LV_PART_MAIN), 128);
    Publish(WidgetPropertyType::IS_ACTIVE, true);
    Publish(WidgetPropertyType::IS_ACTIVE, false);
    zassert_false(lv_obj_has_flag(widget.GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN));
    zassert_equal(lv_obj_get_style_bg_opa(object, LV_PART_MAIN), 128);
}

ZTEST(icon_lifecycle, test_inactive_image_keeps_its_pixels_and_opacity) {
    auto configuration = Configuration(IconType::Image);
    configuration->properties[WidgetPropertyType::IS_ACTIVE] = false;
    TestIcon widget(1, MakeRoot(), ImageContext(*configuration));
    widget.Configure(configuration);
    zassert_equal(widget.Render(), 0);
    widget.OnActivated();
    auto* image = Inner(widget);
    const auto* source = lv_image_get_src(image);
    zassert_not_null(source);
    zassert_equal(lv_obj_get_style_opa(image, LV_PART_MAIN), 255);
    Publish(WidgetPropertyType::IS_ACTIVE, true);
    Publish(WidgetPropertyType::IS_ACTIVE, false);
    ThemeManager::GetInstance().SetTheme(std::make_shared<BlueTheme>());
    zassert_equal(lv_obj_get_style_opa(image, LV_PART_MAIN), 255);
    zassert_equal(lv_image_get_src(image), source);
}

ZTEST(icon_lifecycle, test_dial_needle_inherits_live_owner_state_and_has_one_frame_owner) {
    const auto callbacks = lv_display_get_event_count(lv_display_get_default());
    for(auto target : { WidgetPropertyType::IS_ACTIVE, WidgetPropertyType::IS_VISIBLE, WidgetPropertyType::OPACITY }) {
        {
            auto configuration = Configuration(IconType::Image);
            configuration->properties[target] = target == WidgetPropertyType::OPACITY ? ConfigValue{0} : ConfigValue{false};
            configuration->properties[WidgetPropertyType::VALUE] = 50.0;
            TestDial dial(1, MakeRoot(), ImageContext(*configuration));
            dial.Configure(configuration);
            zassert_equal(dial.Render(), 0);
            dial.OnActivated();
            auto& needle = dial.Needle();
            zassert_true(needle.IsActive());
            zassert_true(needle.IsVisible());
            zassert_false(needle.IsProcessingEligible());
            zassert_equal(dial.GetContainer()->GetChild().get(), needle.GetContainer().get());
            auto* image = needle.GetContainer()->GetChild()->GetObject();
            const auto* source = lv_image_get_src(image);
            Publish(target, 1);
            zassert_true(needle.IsProcessingEligible());
            zassert_equal(lv_image_get_rotation(image), static_cast<int32_t>(dial.GetAngleForValue(50.0F)));
            dial.OnDeactivated();
            zassert_false(needle.IsProcessingEligible());
            dial.OnActivated();
            zassert_true(needle.IsProcessingEligible());
            Publish(target, 0);
            zassert_false(needle.IsProcessingEligible());
            zassert_equal(lv_image_get_src(image), source);
            zassert_equal(lv_obj_get_style_opa(image, LV_PART_MAIN), 255);
        }
        zassert_equal(lv_display_get_event_count(lv_display_get_default()), callbacks);
        lv_refr_now(nullptr);
    }
}
