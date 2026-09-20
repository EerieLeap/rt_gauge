#include <algorithm>
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
#include "views/widgets/basic/icons/shape_icon/oval_icon/oval_icon.h"
#include "views/widgets/indicators/dial_indicator/dial_indicator.h"
#include "views/widgets/widget_factory.h"
#include "domain/ui_domain/utilities/widget_property_validator.h"

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
    icons::OvalIcon* Oval() const { return dynamic_cast<icons::OvalIcon*>(icon_.get()); }
    ConfigValue Read(WidgetPropertyType type) const { return properties_->Get(type); }
};

class TestDial : public indicators::DialIndicator {
public:
    using DialIndicator::DialIndicator;
    IconWidget& Needle() const { return *static_cast<IconWidget*>(dependencies_.front()); }
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
                       WidgetPropertyType::OPACITY, WidgetPropertyType::POSITION_X, WidgetPropertyType::VALUE,
                       WidgetPropertyType::ANIMATION_TYPE, WidgetPropertyType::IS_ANIMATION_ACTIVE,
                       WidgetPropertyType::ANIMATION_DURATION_MS, WidgetPropertyType::COLOR_PRIMARY_ACTIVE }) {
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
    return widget.GetIconContainer()->GetObject();
}

lv_anim_t* Pulse(const TestIcon& widget) {
    return lv_anim_get(widget.Oval(), nullptr);
}

void Tick(uint32_t elapsed) {
    lv_tick_inc(elapsed);
    lv_anim_refr_now();
}

WidgetContext ImageContext(WidgetConfiguration& configuration, int width = 4, int height = 4) {
    using eerie_leap::subsys::device_tree::DtFs;
    using eerie_leap::subsys::fs::services::FsService;
    DtFs::InitInternalFs();
    auto fs = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    zassert_true(fs->Initialize());
    auto assets = std::make_shared<AssetsManager>(fs, "icon-lifecycle");
    std::vector<uint8_t> pixels(width * height * lv_color_format_get_size(LV_COLOR_FORMAT_NATIVE_WITH_ALPHA), 255);
    zassert_true(assets->Save("needle.bin", pixels));
    configuration.properties[WidgetPropertyType::FILE_PATH] = std::pmr::string("needle.bin");
    configuration.properties[WidgetPropertyType::IMG_WIDTH] = width;
    configuration.properties[WidgetPropertyType::IMG_HEIGHT] = height;
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

ZTEST(icon_lifecycle, test_demo_dial_geometry_and_image_rotation_baseline) {
    ScopedLvglLock lock;
    for(bool smoothed : { false, true }) {
        auto configuration = Configuration(IconType::Image);
        configuration->type = WidgetType::IndicatorDial;
        configuration->id = 9;
        configuration->position_grid = { 0, 0 };
        configuration->size_grid = { 466, 466 };
        configuration->z_index = 0;
        configuration->properties[WidgetPropertyType::POSITION_X] = 0;
        configuration->properties[WidgetPropertyType::POSITION_Y] = -104;
        configuration->properties[WidgetPropertyType::ANCHOR_POINT_X] = 7;
        configuration->properties[WidgetPropertyType::ANCHOR_POINT_Y] = 7;
        configuration->properties[WidgetPropertyType::MIN_VALUE] = 0;
        configuration->properties[WidgetPropertyType::MAX_VALUE] = 100;
        configuration->properties[WidgetPropertyType::IS_SMOOTHED] = smoothed;
        auto context = ImageContext(*configuration, 15, 220);
        auto root = std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(466, false).SetHeight(466, false).Build());
        TestDial dial(9, root, context);
        dial.SetSizePx({ 466, 466 });
        dial.Configure(configuration);
        zassert_equal(dial.Render(), 0);
        dial.OnActivated();
        lv_obj_update_layout(root->GetObject());
        auto* image = dial.Needle().GetIconContainer()->GetObject();
        zassert_true(lv_obj_check_type(image, &lv_image_class));
        zassert_equal(lv_obj_get_width(dial.GetContainer()->GetObject()), 466);
        zassert_equal(lv_obj_get_height(dial.GetContainer()->GetObject()), 466);
        zassert_equal(lv_obj_get_width(image), 15);
        zassert_equal(lv_obj_get_height(image), 220);
        zassert_equal(lv_obj_get_x(image), 226, "image x=%d", lv_obj_get_x(image));
        zassert_equal(lv_obj_get_y(image), 19, "image y=%d", lv_obj_get_y(image));
        lv_point_t pivot;
        lv_image_get_pivot(image, &pivot);
        zassert_equal(pivot.x, 7);
        zassert_equal(pivot.y, 213);
        const auto* source = lv_image_get_src(image);
        const struct {
            int value;
            uint32_t mapped_angle;
            int32_t image_angle;
        } cases[] = {
            { 0, UINT32_MAX - 1349U, 2250 },
            { 50, 0, 0 },
            { 100, 1350, 1350 }
        };
        for(const auto& expected : cases) {
            zassert_equal(dial.GetAngleForValue(expected.value), expected.mapped_angle);
            Publish(WidgetPropertyType::VALUE, expected.value);
            Tick(4001);
            zassert_equal(lv_image_get_rotation(image), expected.image_angle);
            zassert_equal(lv_image_get_src(image), source);
            zassert_equal(lv_obj_get_style_transform_rotation(views_test::WidgetContent(dial), LV_PART_MAIN), 0);
            zassert_is_null(lv_anim_get(&dial, nullptr));
        }
    }
}

ZTEST(icon_lifecycle, test_dial_custom_angles_and_default_image_pivot_baseline) {
    ScopedLvglLock lock;
    auto configuration = Configuration(IconType::Image);
    configuration->properties[WidgetPropertyType::START_ANGLE] = 180;
    configuration->properties[WidgetPropertyType::END_ANGLE] = 450;
    auto context = ImageContext(*configuration, 15, 220);
    TestDial dial(1, MakeRoot(), context);
    dial.Configure(configuration);
    zassert_equal(dial.Render(), 0);
    dial.OnActivated();
    auto* image = dial.Needle().GetIconContainer()->GetObject();
    lv_point_t pivot;
    lv_image_get_pivot(image, &pivot);
    zassert_equal(pivot.x, 7);
    zassert_equal(pivot.y, 220);
    for(int value : { 0, 50, 100 }) {
        Publish(WidgetPropertyType::VALUE, value);
        zassert_equal(dial.GetAngleForValue(value), 27 * value);
        zassert_equal(lv_image_get_rotation(image), 27 * value);
    }
}

ZTEST(icon_lifecycle, test_all_factory_widgets_support_live_animation_without_rebuilding) {
    eerie_leap::domain::ui_domain::ScopedLvglLock lock;
    const auto count = lv_anim_count_running();
    const auto callbacks = lv_display_get_event_count(lv_display_get_default());
    auto& factory = WidgetFactory::GetInstance();
    for(auto type : factory.GetAvailableTypes()) {
        const auto icons = type == WidgetType::BasicIcon || type == WidgetType::BasicArcIcon
            ? std::vector<IconType>{ IconType::Label, IconType::Rectangle, IconType::TriangleIsosceles,
                IconType::TriangleRight, IconType::Oval, IconType::Line, IconType::Image }
            : std::vector<IconType>{ IconType::Oval };
        for(auto icon : icons) {
            auto root = MakeRoot();
            auto configuration = Configuration(icon);
            if(icon == IconType::Label)
                configuration->properties[WidgetPropertyType::LABEL] = std::pmr::string("RPM");
            WidgetContext context;
            if(icon == IconType::Image || type == WidgetType::IndicatorDial)
                context = ImageContext(*configuration);
            auto widget = factory.CreateWidget(type, 1, root, context);
            const auto supported = widget->GetSupportedProperties();
            for(auto property : { WidgetPropertyType::ANIMATION_TYPE, WidgetPropertyType::IS_ANIMATION_ACTIVE,
                                 WidgetPropertyType::ANIMATION_DURATION_MS })
                zassert_equal(std::count(supported.begin(), supported.end(), property), 1);
            widget->SetSizePx({ 80, 80 });
            widget->Configure(configuration);
            zassert_equal(widget->Render(), 0, "widget type %d icon %d", static_cast<int>(type), static_cast<int>(icon));
            widget->OnActivated();
            auto* content = views_test::WidgetContent(*widget);
            auto* leaf = lv_obj_get_child(content, 0);
            zassert_equal(lv_anim_count_running(), count);
            Publish(WidgetPropertyType::ANIMATION_TYPE, 1);
            zassert_equal(lv_anim_count_running(), count);
            Publish(WidgetPropertyType::IS_ANIMATION_ACTIVE, true);
            zassert_equal(lv_anim_count_running(), count + 1);
            Tick(500);
            zassert_equal(lv_obj_get_style_opa_layered(content, LV_PART_MAIN), 0);
            Publish(WidgetPropertyType::VALUE, 42.0F);
            Publish(WidgetPropertyType::COLOR_PRIMARY_ACTIVE, std::string("#12345680"));
            ThemeManager::GetInstance().SetTheme(std::make_shared<BlueTheme>());
            lv_display_send_event(lv_display_get_default(), LV_EVENT_REFR_START, nullptr);
            zassert_equal(lv_obj_get_style_opa_layered(content, LV_PART_MAIN), 0);
            zassert_equal(lv_anim_count_running(), count + 1);
            Publish(WidgetPropertyType::ANIMATION_TYPE, 2);
            Tick(250);
            widget->SetSizePx({ 72, 64 });
            lv_obj_update_layout(root->GetObject());
            ThemeManager::GetInstance().SetTheme(std::make_shared<DefaultTheme>());
            zassert_equal(lv_obj_get_style_transform_rotation(content, LV_PART_MAIN), 900);
            zassert_equal(views_test::WidgetContent(*widget), content);
            zassert_equal(lv_obj_get_child(content, 0), leaf);
            Publish(WidgetPropertyType::ANIMATION_DURATION_MS, 2000);
            zassert_equal(lv_obj_get_style_transform_rotation(content, LV_PART_MAIN), 0);
            Tick(500);
            zassert_equal(lv_obj_get_style_transform_rotation(content, LV_PART_MAIN), 900);
            Publish(WidgetPropertyType::IS_VISIBLE, false);
            zassert_equal(lv_anim_count_running(), count);
            Publish(WidgetPropertyType::ANIMATION_TYPE, 1);
            Publish(WidgetPropertyType::ANIMATION_DURATION_MS, 1000);
            Publish(WidgetPropertyType::IS_VISIBLE, true);
            zassert_equal(lv_anim_count_running(), count + 1);
            Tick(500);
            zassert_equal(lv_obj_get_style_opa_layered(content, LV_PART_MAIN), 0);
            widget.reset();
            zassert_equal(lv_anim_count_running(), count);
            zassert_equal(lv_display_get_event_count(lv_display_get_default()), callbacks);
        }
    }
}

ZTEST(icon_lifecycle, test_dial_generic_animation_is_owned_once_and_keeps_needle_value_rotation) {
    eerie_leap::domain::ui_domain::ScopedLvglLock lock;
    const auto count = lv_anim_count_running();
    auto configuration = Configuration(IconType::Image);
    configuration->properties[WidgetPropertyType::ANIMATION_TYPE] = 1;
    configuration->properties[WidgetPropertyType::IS_ANIMATION_ACTIVE] = true;
    configuration->properties[WidgetPropertyType::START_ANGLE] = 180;
    configuration->properties[WidgetPropertyType::END_ANGLE] = 450;
    auto context = ImageContext(*configuration);
    TestDial dial(1, MakeRoot(), context);
    dial.Configure(configuration);
    zassert_equal(dial.Render(), 0);
    dial.OnActivated();
    auto* content = views_test::WidgetContent(dial);
    auto* needle_content = views_test::WidgetContent(dial.Needle());
    auto* image = dial.Needle().GetIconContainer()->GetObject();
    const auto* source = lv_image_get_src(image);
    zassert_equal(lv_anim_count_running(), count + 1);
    Tick(500);
    zassert_equal(lv_obj_get_style_opa_layered(content, LV_PART_MAIN), 0);
    zassert_equal(lv_obj_get_style_opa_layered(needle_content, LV_PART_MAIN), 255);
    zassert_true(dial.Needle().IsProcessingEligible());
    Publish(WidgetPropertyType::VALUE, 50.0F);
    zassert_equal(lv_image_get_rotation(image), 1350);
    Publish(WidgetPropertyType::ANIMATION_TYPE, 2);
    Tick(250);
    zassert_equal(lv_obj_get_style_transform_rotation(content, LV_PART_MAIN), 900);
    zassert_equal(lv_obj_get_style_transform_rotation(needle_content, LV_PART_MAIN), 0);
    zassert_equal(lv_image_get_rotation(image), 1350);
    Publish(WidgetPropertyType::IS_ACTIVE, false);
    zassert_false(dial.Needle().IsTrackingEligible());
    Publish(WidgetPropertyType::VALUE, 80.0F);
    Publish(WidgetPropertyType::IS_ACTIVE, true);
    zassert_equal(lv_image_get_rotation(image), 1350);
    zassert_equal(lv_image_get_src(image), source);
    zassert_equal(lv_anim_count_running(), count + 1);
    TestIcon independent(2, MakeRoot(), context);
    independent.Configure(configuration);
    zassert_equal(independent.Render(), 0);
    independent.OnActivated();
    zassert_equal(lv_anim_count_running(), count + 2);
    Tick(500);
    zassert_equal(lv_obj_get_style_opa_layered(views_test::WidgetContent(independent), LV_PART_MAIN), 0);
    zassert_equal(lv_obj_get_style_opa_layered(needle_content, LV_PART_MAIN), 255);
    dial.OnDeactivated();
    zassert_equal(lv_anim_count_running(), count + 1);
}

ZTEST(icon_lifecycle, test_oval_management_restoration_does_not_start_animation) {
    auto configuration = Configuration(IconType::Oval);
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
    zassert_equal(lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN), 128);
    zassert_true(widget.IsProcessingEligible());
    Tick(200);
    zassert_equal(lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN), 128);
}

ZTEST(icon_lifecycle, test_oval_management_changes_keep_static_appearance) {
    for(auto target : { WidgetPropertyType::IS_ACTIVE, WidgetPropertyType::IS_VISIBLE, WidgetPropertyType::OPACITY }) {
        ThemeManager::GetInstance().SetTheme(std::make_shared<DefaultTheme>());
        TestIcon widget(1, MakeRoot(), WidgetContext{});
        widget.Configure(Configuration(IconType::Oval));
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
        zassert_equal(lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN), 128);
        Publish(target, 1);
        zassert_is_null(Pulse(widget));
        zassert_true(widget.IsProcessingEligible());
        zassert_false(lv_obj_has_flag(widget.GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN));
        zassert_equal(lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN), 128);
        Tick(100);
        zassert_equal(lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN), 128);
    }
}

    ZTEST(icon_lifecycle, test_oval_ancestor_suspension_keeps_static_appearance) {
    for(int gate : { 0, 1, 2 }) {
        auto root = MakeRoot();
        TestIcon widget(1, root, WidgetContext{});
        widget.Configure(Configuration(IconType::Oval));
        zassert_equal(widget.Render(), 0);
        widget.OnActivated();
        Tick(200);
        const auto opacity = lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN);
        zassert_equal(opacity, LV_OPA_COVER);
        if(gate == 0)
            lv_obj_add_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN);
        else if(gate == 1)
            lv_obj_set_style_opa(root->GetObject(), 0, 0);
        else
            lv_obj_set_style_opa_layered(root->GetObject(), 0, 0);
        Tick(50);
        zassert_false(widget.IsProcessingEligible());
        zassert_is_null(Pulse(widget));
        zassert_equal(lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN), opacity);
        lv_obj_remove_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(root->GetObject(), 255, 0);
        lv_obj_set_style_opa_layered(root->GetObject(), 255, 0);
        lv_refr_now(nullptr);
        zassert_true(widget.IsProcessingEligible());
        zassert_is_null(Pulse(widget));
        Tick(100);
        zassert_equal(lv_obj_get_style_opa(Inner(widget), LV_PART_MAIN), opacity);
    }
}

ZTEST(icon_lifecycle, test_icon_eligibility_uses_ancestor_layered_opacity_but_not_color_alpha) {
    class TestOval : public icons::OvalIcon {
    public:
        using OvalIcon::OvalIcon;
        using IconBase::IsProcessingEligible;
    };
    auto root = MakeRoot();
    auto properties = std::make_shared<WidgetPropertyStore>();
    TestOval::RegisterProperties(*properties);
    zassert_true(properties->Set(WidgetPropertyType::COLOR_PRIMARY_ACTIVE, std::pmr::string("#FFFFFF00")));
    TestOval oval(root);
    oval.Configure(properties);
    zassert_equal(oval.Render(), 0);
    oval.SetProcessingEnabled(true);
    zassert_true(oval.IsProcessingEligible());
    lv_obj_set_style_opa_layered(root->GetObject(), 0, LV_PART_MAIN);
    zassert_false(oval.IsProcessingEligible());
    lv_obj_set_style_opa_layered(root->GetObject(), 128, LV_PART_MAIN);
    zassert_true(oval.IsProcessingEligible());
}

ZTEST(icon_lifecycle, test_oval_group_lifecycle_and_teardown_do_not_start_animation) {
    const auto callbacks = lv_display_get_event_count(lv_display_get_default());
    icons::OvalIcon* target = nullptr;
    for(bool suspended : { false, true }) {
        {
            TestIcon widget(1, MakeRoot(), WidgetContext{});
            widget.Configure(Configuration(IconType::Oval));
            zassert_equal(widget.Render(), 0);
            widget.OnActivated();
            target = widget.Oval();
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
    auto configuration = Configuration(IconType::Oval);
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

ZTEST(icon_lifecycle, test_logging_oval_on_arc_preserves_size_position_and_blinking) {
    eerie_leap::domain::ui_domain::ScopedLvglLock lock;
    const auto animations = lv_anim_count_running();
    auto root = MakeRoot();
    auto configuration = Configuration(IconType::Oval);
    configuration->type = WidgetType::BasicArcIcon;
    configuration->properties[WidgetPropertyType::WIDTH_PX] = 16;
    configuration->properties[WidgetPropertyType::HEIGHT_PX] = 16;
    configuration->properties[WidgetPropertyType::IS_VISIBLE] = false;
    configuration->properties[WidgetPropertyType::POSITION_ANGLE] = 180.0F;
    configuration->properties[WidgetPropertyType::EDGE_OFFSET] = 6;
    configuration->properties[WidgetPropertyType::IS_ANIMATION_ACTIVE] = true;
    configuration->properties[WidgetPropertyType::ANIMATION_TYPE] = static_cast<int>(Animation::Type::Blinking);
    configuration->properties[WidgetPropertyType::ANIMATION_DURATION_MS] = 1000;
    configuration->bindings.push_back(PropertyBinding {
        .target = WidgetPropertyType::IS_VISIBLE,
        .channel = EventChannelId::Logging,
        .event_type = std::to_underlying(LoggingEventType::StatusUpdated),
        .payload_key = std::to_underlying(LoggingPayloadType::IsActive)
    });
    auto widget = WidgetFactory::GetInstance().CreateWidget(configuration, root, WidgetContext{});
    widget->SetSizePx({ 100, 100 });
    zassert_equal(widget->Render(), 0);
    widget->OnActivated();
    zassert_equal(lv_anim_count_running(), animations);
    zassert_true(lv_obj_has_flag(widget->GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN));

    PublishLogging(true);
    auto* icon = static_cast<IconWidget*>(widget.get())->GetIconContainer()->GetObject();
    auto* content = views_test::WidgetContent(*widget);
    lv_obj_update_layout(root->GetObject());
    zassert_equal(lv_obj_get_width(icon), 16);
    zassert_equal(lv_obj_get_height(icon), 16);
    zassert_within(lv_obj_get_style_x(icon, LV_PART_MAIN), 0, 1);
    zassert_within(lv_obj_get_style_y(icon, LV_PART_MAIN), -36, 1);
    const auto* mask = static_cast<const lv_image_dsc_t*>(lv_image_get_src(icon));
    zassert_not_null(mask);
    zassert_within(mask->data[8 * mask->header.stride + 8], LV_OPA_COVER, 1);
    zassert_equal(mask->data[0], LV_OPA_TRANSP);
    zassert_false(lv_obj_has_flag(widget->GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN));
    zassert_equal(lv_anim_count_running(), animations + 1);
    Tick(500);
    zassert_equal(lv_obj_get_style_opa_layered(content, LV_PART_MAIN), LV_OPA_TRANSP);
    Tick(500);
    zassert_equal(lv_obj_get_style_opa_layered(content, LV_PART_MAIN), LV_OPA_COVER);
    PublishLogging(false);
    zassert_true(lv_obj_has_flag(widget->GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN));
    zassert_equal(lv_anim_count_running(), animations);
    PublishLogging(true);
    zassert_equal(lv_anim_count_running(), animations + 1);
    widget.reset();
    zassert_equal(lv_anim_count_running(), animations);
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
            zassert_equal(dial.GetContainer()->GetChild()->GetChild().get(), needle.GetContainer().get());
            auto* image = needle.GetIconContainer()->GetObject();
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

ZTEST(icon_lifecycle, test_dial_value_still_rotates_image_under_owner_presentation_transform) {
    ScopedLvglLock lock;
    auto configuration = Configuration(IconType::Image);
    configuration->properties[WidgetPropertyType::START_ANGLE] = 180;
    configuration->properties[WidgetPropertyType::END_ANGLE] = 450;
    TestDial dial(1, MakeRoot(), ImageContext(*configuration));
    zassert_is_null(dial.Needle().GetIconContainer());
    dial.Configure(configuration);
    zassert_equal(dial.Render(), 0);
    dial.OnActivated();
    auto* image = dial.Needle().GetIconContainer()->GetObject();
    zassert_true(lv_obj_check_type(image, &lv_image_class));
    const auto* source = lv_image_get_src(image);
    auto* presentation = views_test::WidgetContent(dial);
    auto* needle_presentation = views_test::WidgetContent(dial.Needle());
    for(int angle : { 450, 900 }) {
        lv_obj_set_style_transform_rotation(presentation, angle, LV_PART_MAIN);
        for(int opacity : { 255, 0 }) {
            lv_obj_set_style_opa_layered(presentation, opacity, LV_PART_MAIN);
            for(int value : { 25, 75 }) {
                Publish(WidgetPropertyType::VALUE, value);
                zassert_equal(lv_image_get_rotation(image), dial.GetAngleForValue(value));
                zassert_equal(lv_obj_get_style_transform_rotation(presentation, LV_PART_MAIN), angle);
                zassert_equal(lv_obj_get_style_transform_rotation(needle_presentation, LV_PART_MAIN), 0);
                zassert_equal(lv_image_get_src(image), source);
                zassert_true(dial.Needle().IsProcessingEligible());
                zassert_is_null(lv_anim_get(&dial, nullptr));
            }
        }
    }
}

ZTEST(icon_lifecycle, test_images_and_dials_apply_opacity_once_without_recoloring_or_reloading) {
    for(bool is_dial : { false, true }) {
        auto configuration = Configuration(IconType::Image);
        configuration->properties[WidgetPropertyType::OPACITY] = 128;
        if(is_dial)
            configuration->properties[WidgetPropertyType::START_ANGLE] = 180;
        auto root = MakeRoot();
        auto context = ImageContext(*configuration);
        std::unique_ptr<WidgetBase> widget;
        if(is_dial)
            widget = std::make_unique<TestDial>(1, root, context);
        else
            widget = std::make_unique<TestIcon>(1, root, context);
        widget->Configure(configuration);
        auto* outer = widget->GetContainer()->GetObject();
        zassert_equal(lv_obj_get_style_opa_layered(outer, LV_PART_MAIN), 128);
        zassert_equal(widget->Render(), 0);
        widget->OnActivated();
        const auto supported = widget->GetSupportedProperties();
        for(auto property : supported)
            zassert_false(eerie_leap::domain::ui_domain::utilities::WidgetPropertyValidator::IsColorProperty(property));

        auto* image = lv_obj_get_child(views_test::WidgetContent(*widget), 0);
        if(is_dial) {
            zassert_equal(lv_obj_get_style_opa(image, LV_PART_MAIN), LV_OPA_COVER);
            image = static_cast<TestDial*>(widget.get())->Needle().GetIconContainer()->GetObject();
        }
        const auto* source = lv_image_get_src(image);
        zassert_not_null(source);
        lv_obj_set_style_bg_color(root->GetObject(), lv_color_black(), 0);
        lv_obj_set_style_bg_opa(root->GetObject(), LV_OPA_COVER, 0);
        lv_obj_update_layout(root->GetObject());

        for(int opacity : { 128, 0, 255 }) {
            Publish(WidgetPropertyType::OPACITY, opacity);
            ThemeManager::GetInstance().SetTheme(std::make_shared<BlueTheme>());
            widget->OnDeactivated();
            widget->OnActivated();
            Tick(100);
            zassert_equal(lv_obj_get_style_opa_layered(outer, LV_PART_MAIN), opacity);
            zassert_equal(lv_obj_get_style_opa(image, LV_PART_MAIN), LV_OPA_COVER);
            zassert_equal(lv_obj_get_style_image_recolor_opa(image, LV_PART_MAIN), LV_OPA_TRANSP);
            zassert_equal(lv_image_get_src(image), source);
            zassert_equal(widget->IsProcessingEligible(), opacity > 0);
            if(is_dial) {
                auto& needle = static_cast<TestDial*>(widget.get())->Needle();
                zassert_equal(lv_obj_get_style_opa_layered(needle.GetContainer()->GetObject(), LV_PART_MAIN), LV_OPA_COVER);
                zassert_equal(needle.IsProcessingEligible(), opacity > 0);
            }

            std::unique_ptr<lv_draw_buf_t, decltype(&lv_draw_buf_destroy)> snapshot(
                lv_snapshot_take(root->GetObject(), LV_COLOR_FORMAT_ARGB8888), lv_draw_buf_destroy);
            zassert_not_null(snapshot);
            int brightest = 0;
            for(uint32_t y = 0; y < snapshot->header.h; ++y) {
                auto* pixels = reinterpret_cast<const lv_color32_t*>(snapshot->data + y * snapshot->header.stride);
                for(uint32_t x = 0; x < snapshot->header.w; ++x)
                    brightest = std::max(brightest, static_cast<int>(pixels[x].red));
            }
            // White image pixels on black must fade once: 128, never the double-faded ~64.
            // Allow one RGB565 red-channel quantization step in the intermediate layer.
            zassert_within(brightest, opacity, 8, "dial=%d opacity=%d brightest=%d", is_dial, opacity, brightest);
        }
    }
}
