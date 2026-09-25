#include <algorithm>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

#include <zephyr/ztest.h>

#include "subsys/assets/assets_manager.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "domain/logging_domain/event_bus/logging_events_channel.h"
#include "domain/sensor_domain/event_bus/sensor_events_channel.h"
#include "domain/ui_domain/configuration/parsers/ui_configuration_validator.h"
#include "event_bus/event_channel_id.h"
#include "event_bus/event_channels.h"
#include "views/screens/screen.h"
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
using eerie_leap::domain::ui_domain::configuration::parsers::UiConfigurationValidator;
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
    IconWidget& Needle() const { return static_cast<IconWidget&>(*GetChildren().front()); }

    void Assemble(std::shared_ptr<WidgetConfiguration> dial, std::shared_ptr<WidgetConfiguration> needle,
        const WidgetContext& context) {
        views_test::InjectChildren(*this, *dial, { std::move(needle) }, context);
        Configure(std::move(dial));
    }
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
                       WidgetPropertyType::ANIMATION_DURATION_MS, WidgetPropertyType::COLOR_PRIMARY_ACTIVE,
                       WidgetPropertyType::ANCHOR_POINT_X, WidgetPropertyType::ANCHOR_POINT_Y }) {
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

// The dial keeps the shared test bindings; image properties belong to its needle.
std::shared_ptr<WidgetConfiguration> DialConfiguration(uint32_t id = 1) {
    auto configuration = Configuration(IconType::Image);
    configuration->properties.erase(WidgetPropertyType::ICON_TYPE);
    configuration->type = WidgetType::IndicatorDial;
    configuration->id = id;
    return configuration;
}

// Factory-built dials need a needle before they are configured.
void InjectTestNeedle(IWidget& widget, WidgetConfiguration& configuration) {
    if(widget.GetType() != WidgetType::IndicatorDial)
        return;
    configuration.id = widget.GetId();
    configuration.type = widget.GetType();
    views_test::InjectChildren(widget, configuration, { views_test::NeedleConfiguration(widget.GetId() + 1) });
}

// Screens build only what UiConfigurationManager already validated with these rules.
void ValidateScreen(const ScreenConfiguration& screen) {
    UiConfigurationValidator::Validate(screen, [](const auto& owner, auto children) {
        WidgetFactory::GetInstance().ValidateChildren(owner, children);
    });
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
    // RGB565A8 reports the RGB plane's two bytes per pixel; append the separate alpha plane.
    const int bytes_per_pixel = lv_color_format_get_size(LV_COLOR_FORMAT_NATIVE_WITH_ALPHA)
        + (LV_COLOR_FORMAT_NATIVE_WITH_ALPHA == LV_COLOR_FORMAT_RGB565A8 ? 1 : 0);
    std::vector<uint8_t> pixels(width * height * bytes_per_pixel, 255);
    zassert_true(assets->Save("needle.bin", pixels));
    configuration.properties[WidgetPropertyType::FILE_PATH] = std::pmr::string("needle.bin");
    configuration.properties[WidgetPropertyType::IMG_WIDTH] = width;
    configuration.properties[WidgetPropertyType::IMG_HEIGHT] = height;
    return WidgetContext { .assets_manager = std::move(assets) };
}

constexpr std::array native_rotation_icons {
    IconType::Image, IconType::Rectangle, IconType::TriangleIsosceles,
    IconType::TriangleRight, IconType::Oval, IconType::Line
};

std::shared_ptr<WidgetConfiguration> RotationConfiguration(IconType type) {
    auto configuration = Configuration(type);
    std::erase_if(configuration->bindings, [](const auto& binding) {
        return binding.target == WidgetPropertyType::ANIMATION_TYPE;
    });
    configuration->properties[WidgetPropertyType::WIDTH_PX] = 20;
    configuration->properties[WidgetPropertyType::HEIGHT_PX] = 60;
    return configuration;
}

void CheckRotation(const IconWidget& widget, int angle, int x, int y) {
    zassert_equal(lv_image_get_rotation(Inner(widget)), angle);
    lv_point_t pivot;
    lv_image_get_pivot(Inner(widget), &pivot);
    zassert_equal(pivot.x, x);
    zassert_equal(pivot.y, y);
    // Native rotation must not also rotate either surrounding layer.
    zassert_equal(lv_obj_get_style_transform_rotation(Inner(widget), LV_PART_MAIN), 0);
    zassert_equal(lv_obj_get_style_transform_rotation(views_test::WidgetContent(widget), LV_PART_MAIN), 0);
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

ZTEST(icon_lifecycle, test_rotation_retains_signed_angles_anchors_and_rerender) {
    ScopedLvglLock lock;
    for(auto type : native_rotation_icons) {
        auto configuration = RotationConfiguration(type);
        configuration->properties[WidgetPropertyType::POSITION_X] = 13;
        configuration->properties[WidgetPropertyType::POSITION_Y] = -17;
        auto context = type == IconType::Image ? ImageContext(*configuration, 20, 60) : WidgetContext{};
        TestIcon widget(1, MakeRoot(), context);
        widget.Configure(configuration);
        IWidget& abstract_widget = widget;
        IWidget* rotation = &abstract_widget;
        zassert_true(rotation->SetRotation(-4500));
        zassert_equal(widget.Render(), 0);
        CheckRotation(widget, 2700, 10, 30);
        widget.OnActivated();
        CheckRotation(widget, 2700, 10, 30);
        for(int32_t angle : std::initializer_list<int32_t> { 0, -1, 4500, -7200, INT32_MIN, INT32_MAX }) {
            zassert_true(rotation->SetRotation(angle));
            CheckRotation(widget, (angle % 3600 + 3600) % 3600, 10, 30);
        }
        zassert_true(rotation->SetRotation(900));
        Publish(WidgetPropertyType::ANCHOR_POINT_X, 3);
        Publish(WidgetPropertyType::ANCHOR_POINT_Y, 7);
        CheckRotation(widget, 900, 3, 53);
        auto* presentation = views_test::WidgetContent(widget);
        const int x = lv_obj_get_x(Inner(widget));
        const int y = lv_obj_get_y(Inner(widget));
        zassert_equal(lv_obj_get_style_transform_pivot_x(presentation, LV_PART_MAIN), x + 3);
        zassert_equal(lv_obj_get_style_transform_pivot_y(presentation, LV_PART_MAIN), y + 53);
        zassert_equal(widget.Render(), 0);
        CheckRotation(widget, 900, 3, 53);
        Publish(WidgetPropertyType::ANCHOR_POINT_X, -1);
        Publish(WidgetPropertyType::ANCHOR_POINT_Y, 75);
        CheckRotation(widget, 900, 10, -15);
    }
}

ZTEST(icon_lifecycle, test_rotation_replays_suspended_updates_and_blinking) {
    ScopedLvglLock lock;
    for(auto type : native_rotation_icons) {
        auto configuration = RotationConfiguration(type);
        configuration->properties[WidgetPropertyType::ANIMATION_TYPE] = static_cast<int>(Animation::Type::Blinking);
        configuration->properties[WidgetPropertyType::IS_ANIMATION_ACTIVE] = true;
        configuration->properties[WidgetPropertyType::ANIMATION_DURATION_MS] = 1000;
        auto context = type == IconType::Image ? ImageContext(*configuration, 20, 60) : WidgetContext{};
        TestIcon widget(1, MakeRoot(), context);
        widget.Configure(configuration);
        IWidget* rotation = &widget;
        zassert_equal(widget.Render(), 0);
        widget.OnActivated();
        zassert_true(rotation->SetRotation(900));
        Tick(250);
        CheckRotation(widget, 900, 10, 30);
        zassert_true(lv_obj_get_style_opa_layered(views_test::WidgetContent(widget), LV_PART_MAIN) < LV_OPA_COVER);
        Publish(WidgetPropertyType::IS_VISIBLE, false);
        zassert_true(rotation->SetRotation(-900));
        Publish(WidgetPropertyType::ANCHOR_POINT_Y, 5);
        CheckRotation(widget, 900, 10, 30);
        Publish(WidgetPropertyType::IS_VISIBLE, true);
        CheckRotation(widget, 2700, 10, 55);
        widget.OnDeactivated();
        zassert_true(rotation->SetRotation(450));
        CheckRotation(widget, 2700, 10, 55);
        zassert_equal(widget.Render(), 0);
        CheckRotation(widget, 450, 10, 55);
        widget.OnActivated();
        CheckRotation(widget, 450, 10, 55);
        Publish(WidgetPropertyType::IS_ANIMATION_ACTIVE, false);
        zassert_equal(widget.Render(), 0);
        CheckRotation(widget, 450, 10, 55);
    }
}

ZTEST(icon_lifecycle, test_rotation_resize_preserves_angle_and_resolves_new_bounds) {
    ScopedLvglLock lock;
    for(auto type : native_rotation_icons) {
        auto configuration = RotationConfiguration(type);
        auto context = type == IconType::Image ? ImageContext(*configuration, 20, 60) : WidgetContext{};
        const auto width = type == IconType::Image ? WidgetPropertyType::IMG_WIDTH : WidgetPropertyType::WIDTH_PX;
        const auto height = type == IconType::Image ? WidgetPropertyType::IMG_HEIGHT : WidgetPropertyType::HEIGHT_PX;
        for(auto target : { width, height }) {
            auto binding = configuration->bindings.front();
            binding.target = target;
            binding.selector_value = static_cast<int>(target);
            configuration->bindings.push_back(binding);
        }
        TestIcon widget(1, MakeRoot(), context);
        widget.Configure(configuration);
        IWidget* rotation = &widget;
        zassert_equal(widget.Render(), 0);
        widget.OnActivated();
        zassert_true(rotation->SetRotation(1350));
        Publish(width, 10);
        Publish(height, 30);
        CheckRotation(widget, 1350, 5, 15);
        Publish(WidgetPropertyType::ANCHOR_POINT_Y, 7);
        Publish(height, 40);
        CheckRotation(widget, 1350, 5, 33);
    }
}

ZTEST(icon_lifecycle, test_rotation_survives_failed_image_render) {
    ScopedLvglLock lock;
    auto configuration = RotationConfiguration(IconType::Image);
    auto context = ImageContext(*configuration, 20, 60);
    configuration->properties[WidgetPropertyType::FILE_PATH] = std::pmr::string();
    TestIcon widget(1, MakeRoot(), context);
    widget.Configure(configuration);
    IWidget* rotation = &widget;
    zassert_true(rotation->SetRotation(-900));
    widget.OnActivated();
    zassert_not_equal(widget.Render(), 0);
    zassert_true(rotation->SetRotation(450));
    configuration->properties[WidgetPropertyType::FILE_PATH] = std::pmr::string("needle.bin");
    widget.Configure(configuration);
    zassert_equal(widget.Render(), 0);
    CheckRotation(widget, 450, 10, 30);
}

ZTEST(icon_lifecycle, test_rotation_rejects_conflicting_animation) {
    ScopedLvglLock lock;
    for(auto type : { IconType::Label, IconType::Image, IconType::TriangleIsosceles }) {
        for(bool active : { false, true }) {
            auto configuration = RotationConfiguration(type);
            configuration->properties[WidgetPropertyType::ANIMATION_TYPE] = static_cast<int>(Animation::Type::Rotation);
            configuration->properties[WidgetPropertyType::IS_ANIMATION_ACTIVE] = active;
            TestIcon widget(1, MakeRoot(), WidgetContext{});
            widget.Configure(configuration);
            zassert_false(static_cast<IWidget&>(widget).SetRotation(900));
        }
        TestIcon widget(1, MakeRoot(), WidgetContext{});
        widget.Configure(RotationConfiguration(type));
        IWidget& rotation = widget;
        zassert_true(rotation.SetRotation(450));
        // An inbound animation-type binding could select a conflicting rotation later.
        widget.Configure(Configuration(type));
        zassert_false(rotation.SetRotation(900));
    }
}

ZTEST(icon_lifecycle, test_every_factory_widget_and_icon_supports_direct_rotation) {
    ScopedLvglLock lock;
    const auto animations = lv_anim_count_running();
    auto& factory = WidgetFactory::GetInstance();
    for(auto type : factory.GetAvailableTypes()) {
        const bool is_icon = type == WidgetType::BasicIcon || type == WidgetType::BasicArcIcon;
        const auto icons = is_icon
            ? std::vector<IconType>{ IconType::Label, IconType::Image, IconType::Rectangle,
                IconType::TriangleIsosceles, IconType::TriangleRight, IconType::Oval, IconType::Line }
            : std::vector<IconType>{ IconType::Label };
        for(auto icon : icons) {
            auto configuration = RotationConfiguration(icon);
            configuration->properties[WidgetPropertyType::LABEL] = std::pmr::string("RPM");
            configuration->properties[WidgetPropertyType::ANCHOR_POINT_X] = 3;
            configuration->properties[WidgetPropertyType::ANCHOR_POINT_Y] = 7;
            configuration->properties[WidgetPropertyType::ANIMATION_TYPE] = static_cast<int>(Animation::Type::Blinking);
            configuration->properties[WidgetPropertyType::ANIMATION_DURATION_MS] = 1000;
            WidgetContext context;
            if(icon == IconType::Image)
                context = ImageContext(*configuration, 20, 60);
            auto widget = factory.CreateWidget(type, 1, MakeRoot(), context);
            widget->SetSizePx({ 80, 80 });
            zassert_true(widget->SetRotation(-4500));
            InjectTestNeedle(*widget, *configuration);
            widget->Configure(configuration);
            zassert_equal(widget->Render(), 0);
            const bool native = is_icon && icon != IconType::Label;
            auto angle = [&] {
                auto* content = views_test::WidgetContent(*widget);
                return native ? lv_image_get_rotation(lv_obj_get_child(content, 0))
                    : lv_obj_get_style_transform_rotation(content, LV_PART_MAIN);
            };
            zassert_equal(angle(), 2700, "widget %d icon %d", static_cast<int>(type), static_cast<int>(icon));
            widget->OnActivated();
            for(int32_t value : std::initializer_list<int32_t> { INT32_MIN, INT32_MAX, -1, 0, 4500 }) {
                zassert_true(widget->SetRotation(value));
                zassert_equal(angle(), (value % 3600 + 3600) % 3600);
            }
            Publish(WidgetPropertyType::IS_ANIMATION_ACTIVE, true);
            Tick(250);
            zassert_equal(angle(), 900);
            Publish(WidgetPropertyType::IS_VISIBLE, false);
            zassert_true(widget->SetRotation(450));
            zassert_equal(angle(), 900);
            Publish(WidgetPropertyType::IS_VISIBLE, true);
            zassert_equal(angle(), 450);
            Publish(WidgetPropertyType::ANCHOR_POINT_X, 11);
            widget->SetSizePx({ 72, 64 });
            zassert_equal(angle(), 450);
            Publish(WidgetPropertyType::IS_ANIMATION_ACTIVE, false);
            zassert_equal(angle(), 450);
            zassert_equal(widget->Render(), 0);
            zassert_equal(angle(), 450);
            zassert_true(widget->SetRotation(0));
            zassert_equal(angle(), 0);
        }
    }
    zassert_equal(lv_anim_count_running(), animations);
}

ZTEST(icon_lifecycle, test_rotation_pixels_clipping_and_parent_animation) {
    ScopedLvglLock lock;
    using eerie_leap::views::animations::ViewAnimator;
    for(auto type : { IconType::Image, IconType::TriangleIsosceles }) {
        auto root = MakeRoot();
        root->SetWidth(96, true).SetHeight(96, true);
        lv_obj_set_style_bg_color(root->GetObject(), lv_color_black(), 0);
        lv_obj_set_style_bg_opa(root->GetObject(), LV_OPA_COVER, 0);
        auto frame = [](const std::shared_ptr<Frame>& parent) {
            return std::make_shared<Frame>(Frame::CreateWrapped(parent->GetObject())
                .SetWidth(96, true).SetHeight(96, true).Build());
        };
        auto clip = frame(root);
        auto layout = frame(clip);
        auto presentation = frame(layout);
        lv_obj_set_style_transform_pivot_x(presentation->GetObject(), 48, 0);
        lv_obj_set_style_transform_pivot_y(presentation->GetObject(), 48, 0);
        auto configuration = RotationConfiguration(type);
        configuration->properties[WidgetPropertyType::WIDTH_PX] = 10;
        configuration->properties[WidgetPropertyType::HEIGHT_PX] = 40;
        configuration->properties[WidgetPropertyType::ANCHOR_POINT_Y] = 5;
        configuration->properties[WidgetPropertyType::COLOR_PRIMARY_ACTIVE] = std::pmr::string("#FFFFFFFF");
        auto context = type == IconType::Image ? ImageContext(*configuration, 10, 40) : WidgetContext{};
        TestIcon widget(1, presentation, context);
        widget.Configure(configuration);
        zassert_equal(widget.Render(), 0);
        widget.OnActivated();
        IWidget* rotation = &widget;
        int sample = 0;
        auto check_pixel = [&](int x, int y, bool lit) {
            ++sample;
            lv_obj_update_layout(root->GetObject());
            std::unique_ptr<lv_draw_buf_t, decltype(&lv_draw_buf_destroy)> snapshot(
                lv_snapshot_take(root->GetObject(), LV_COLOR_FORMAT_ARGB8888), lv_draw_buf_destroy);
            zassert_not_null(snapshot);
            auto* row = reinterpret_cast<const lv_color32_t*>(snapshot->data + y * snapshot->header.stride);
            zassert_equal(row[x].red > 200, lit, "type %u sample %d pixel %d,%d = %u",
                static_cast<unsigned>(type), sample, x, y, row[x].red);
        };
        zassert_true(rotation->SetRotation(0));
        check_pixel(48, 35, true);
        zassert_true(rotation->SetRotation(900));
        check_pixel(48, 35, false);
        check_pixel(68, 63, true); // Outside the original 10-pixel drawable width.
        clip->SetWidth(65, true);
        check_pixel(68, 63, false);
        check_pixel(60, 63, true);
        clip->SetWidth(96, true);
        ViewAnimator animator;
        zassert_true(animator.Attach(*presentation, *layout, [](void*) { return true; }, nullptr));
        animator.Synchronize({ Animation::Type::Rotation, true, 1000 });
        Tick(250);
        zassert_within(lv_obj_get_style_transform_rotation(presentation->GetObject(), LV_PART_MAIN), 900, 5);
        CheckRotation(widget, 900, 5, 35);
        check_pixel(33, 68, true);
        check_pixel(68, 63, false);
        animator.StopAndReset();
        check_pixel(68, 63, true);
        zassert_equal(widget.Render(), 0);
        check_pixel(68, 63, true);
    }
}

ZTEST(icon_lifecycle, test_anchor_uses_drawable_size_and_placement_not_screen_size) {
    ScopedLvglLock lock;
    for(auto type : { IconType::Image, IconType::TriangleIsosceles }) {
        for(int extent : { 300, 466 }) {
            auto root = MakeRoot();
            root->SetWidth(extent, true).SetHeight(extent, true);
            auto configuration = Configuration(type);
            configuration->properties[WidgetPropertyType::WIDTH_PX] = 20;
            configuration->properties[WidgetPropertyType::HEIGHT_PX] = 200;
            configuration->properties[WidgetPropertyType::POSITION_X] = 13;
            configuration->properties[WidgetPropertyType::POSITION_Y] = -17;
            const auto context = type == IconType::Image ? ImageContext(*configuration, 20, 200) : WidgetContext{};
            TestIcon widget(1, root, context);
            widget.Configure(configuration);
            zassert_equal(widget.Render(), 0);
            widget.OnActivated();
            auto* image = Inner(widget);
            auto* presentation = views_test::WidgetContent(widget);
            zassert_equal(lv_obj_get_style_transform_pivot_x(image, LV_PART_MAIN), 10);
            zassert_equal(lv_obj_get_style_transform_pivot_y(image, LV_PART_MAIN), 100);
            zassert_equal(lv_obj_get_style_transform_pivot_x(presentation, LV_PART_MAIN), extent / 2 + 13);
            zassert_equal(lv_obj_get_style_transform_pivot_y(presentation, LV_PART_MAIN), extent / 2 - 17);
            if(type == IconType::Image) {
                lv_point_t pivot;
                lv_image_get_pivot(image, &pivot);
                zassert_equal(pivot.x, 10);
                zassert_equal(pivot.y, 100);
            }
            Publish(WidgetPropertyType::ANCHOR_POINT_X, 0);
            for(int coordinate : { 0, 200 }) {
                Publish(WidgetPropertyType::ANCHOR_POINT_Y, coordinate);
                zassert_equal(lv_obj_get_style_transform_pivot_x(image, LV_PART_MAIN), 0);
                zassert_equal(lv_obj_get_style_transform_pivot_y(image, LV_PART_MAIN), 200 - coordinate);
                zassert_equal(lv_obj_get_style_transform_pivot_x(presentation, LV_PART_MAIN), extent / 2 + 3);
                zassert_equal(lv_obj_get_style_transform_pivot_y(presentation, LV_PART_MAIN), extent / 2 + 83 - coordinate);
                if(type == IconType::Image) {
                    lv_point_t pivot;
                    lv_image_get_pivot(image, &pivot);
                    zassert_equal(pivot.x, 0);
                    zassert_equal(pivot.y, 200 - coordinate);
                }
            }
            Publish(WidgetPropertyType::ANCHOR_POINT_Y, -1);
            Publish(WidgetPropertyType::ANCHOR_POINT_X, 3);
            zassert_equal(lv_obj_get_style_transform_pivot_x(image, LV_PART_MAIN), 3);
            zassert_equal(lv_obj_get_style_transform_pivot_y(image, LV_PART_MAIN), 100);
            zassert_equal(lv_obj_get_style_transform_pivot_x(presentation, LV_PART_MAIN), extent / 2 + 6);
            Publish(WidgetPropertyType::ANCHOR_POINT_X, -1);
            Publish(WidgetPropertyType::ANCHOR_POINT_Y, 230);
            zassert_equal(lv_obj_get_style_transform_pivot_x(image, LV_PART_MAIN), 10);
            zassert_equal(lv_obj_get_style_transform_pivot_y(image, LV_PART_MAIN), -30);
            zassert_equal(lv_obj_get_style_transform_pivot_y(presentation, LV_PART_MAIN), extent / 2 - 147);
            if(type == IconType::Image) {
                lv_point_t pivot;
                lv_image_get_pivot(image, &pivot);
                zassert_equal(pivot.y, -30);
            }
        }
    }
}

ZTEST(icon_lifecycle, test_anchor_binding_preserves_angles_and_replays_after_suspension_and_rerender) {
    ScopedLvglLock lock;
    auto configuration = Configuration(IconType::Image);
    configuration->properties[WidgetPropertyType::ANIMATION_TYPE] = static_cast<int>(Animation::Type::Rotation);
    configuration->properties[WidgetPropertyType::IS_ANIMATION_ACTIVE] = true;
    configuration->properties[WidgetPropertyType::ANIMATION_DURATION_MS] = 1000;
    auto context = ImageContext(*configuration, 20, 200);
    auto root = MakeRoot();
    TestIcon widget(1, root, context);
    widget.Configure(configuration);
    Publish(WidgetPropertyType::ANCHOR_POINT_X, 3);
    Publish(WidgetPropertyType::ANCHOR_POINT_Y, 170);
    widget.OnActivated();
    zassert_equal(widget.Render(), 0);
    auto* image = Inner(widget);
    auto* presentation = views_test::WidgetContent(widget);
    zassert_equal(lv_obj_get_style_transform_pivot_x(image, LV_PART_MAIN), 3);
    zassert_equal(lv_obj_get_style_transform_pivot_y(image, LV_PART_MAIN), 30);
    const auto* source = lv_image_get_src(image);
    const auto callbacks = lv_obj_get_event_count(image);
    lv_image_set_rotation(image, 900);
    Tick(250);
    const auto angle = lv_obj_get_style_transform_rotation(presentation, LV_PART_MAIN);
    const auto animations = lv_anim_count_running();
    zassert_true(angle > 0);
    Publish(WidgetPropertyType::ANCHOR_POINT_X, uint32_t { 25 });
    Publish(WidgetPropertyType::ANCHOR_POINT_Y, 211);
    zassert_equal(lv_obj_get_style_transform_rotation(presentation, LV_PART_MAIN), angle);
    zassert_equal(lv_image_get_rotation(image), 900);
    zassert_equal(lv_anim_count_running(), animations);
    zassert_equal(Inner(widget), image);
    zassert_equal(lv_image_get_src(image), source);
    lv_point_t pivot;
    lv_image_get_pivot(image, &pivot);
    zassert_equal(pivot.x, 25);
    zassert_equal(pivot.y, -11);
    const EventData invalid[] = { -2, static_cast<int>(INT32_MAX), uint32_t { UINT32_MAX }, 10.0F, true, std::string("10") };
    for(const auto& value : invalid) {
        Publish(WidgetPropertyType::ANCHOR_POINT_X, value);
        zassert_equal(std::get<int>(widget.Read(WidgetPropertyType::ANCHOR_POINT_X)), 25);
        zassert_equal(lv_image_get_rotation(image), 900);
    }
    lv_obj_add_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN);
    lv_display_send_event(lv_display_get_default(), LV_EVENT_REFR_START, nullptr);
    Publish(WidgetPropertyType::ANCHOR_POINT_X, 17);
    Publish(WidgetPropertyType::ANCHOR_POINT_Y, 191);
    lv_display_send_event(lv_display_get_default(), LV_EVENT_REFR_START, nullptr);
    lv_image_get_pivot(image, &pivot);
    zassert_equal(pivot.x, 25);
    zassert_equal(pivot.y, -11);
    lv_obj_remove_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN);
    lv_display_send_event(lv_display_get_default(), LV_EVENT_REFR_START, nullptr);
    lv_image_get_pivot(image, &pivot);
    zassert_equal(pivot.x, 17);
    zassert_equal(pivot.y, 9);
    zassert_equal(lv_image_get_rotation(image), 900);
    zassert_equal(widget.Render(), 0);
    image = Inner(widget);
    lv_image_get_pivot(image, &pivot);
    zassert_equal(pivot.x, 17);
    zassert_equal(pivot.y, 9);
    zassert_equal(lv_obj_get_event_count(image), callbacks);
}

ZTEST(icon_lifecycle, test_shape_anchor_tracks_live_geometry_and_placement) {
    ScopedLvglLock lock;
    auto configuration = Configuration(IconType::TriangleIsosceles);
    configuration->properties[WidgetPropertyType::WIDTH_PX] = 20;
    configuration->properties[WidgetPropertyType::HEIGHT_PX] = 200;
    for(auto target : { WidgetPropertyType::WIDTH_PX, WidgetPropertyType::HEIGHT_PX }) {
        auto binding = configuration->bindings.front();
        binding.target = target;
        binding.selector_value = static_cast<int>(target);
        configuration->bindings.push_back(binding);
    }
    TestIcon widget(1, MakeRoot(), {});
    widget.Configure(configuration);
    zassert_equal(widget.Render(), 0);
    widget.OnActivated();
    auto* shape = Inner(widget);
    lv_obj_set_style_transform_rotation(shape, 900, LV_PART_MAIN);
    Publish(WidgetPropertyType::WIDTH_PX, 40);
    Publish(WidgetPropertyType::HEIGHT_PX, 80);
    zassert_equal(Inner(widget), shape);
    zassert_equal(lv_obj_get_style_transform_pivot_x(shape, LV_PART_MAIN), 20);
    zassert_equal(lv_obj_get_style_transform_pivot_y(shape, LV_PART_MAIN), 40);
    Publish(WidgetPropertyType::ANCHOR_POINT_X, 5);
    Publish(WidgetPropertyType::WIDTH_PX, 60);
    Publish(WidgetPropertyType::POSITION_X, 12);
    zassert_equal(lv_obj_get_style_transform_pivot_x(shape, LV_PART_MAIN), 5);
    zassert_equal(lv_obj_get_style_transform_pivot_y(shape, LV_PART_MAIN), 40);
    zassert_equal(lv_obj_get_style_transform_pivot_x(views_test::WidgetContent(widget), LV_PART_MAIN), 37);
    zassert_equal(lv_obj_get_style_transform_rotation(shape, LV_PART_MAIN), 900);
}

ZTEST(icon_lifecycle, test_image_anchor_tracks_live_dimensions_without_reloading_or_resetting_angle) {
    ScopedLvglLock lock;
    auto configuration = Configuration(IconType::Image);
    auto context = ImageContext(*configuration, 20, 200);
    for(auto target : { WidgetPropertyType::IMG_WIDTH, WidgetPropertyType::IMG_HEIGHT }) {
        auto binding = configuration->bindings.front();
        binding.target = target;
        binding.selector_value = static_cast<int>(target);
        configuration->bindings.push_back(binding);
    }
    TestIcon widget(1, MakeRoot(), context);
    widget.Configure(configuration);
    zassert_equal(widget.Render(), 0);
    widget.OnActivated();
    auto* image = Inner(widget);
    const auto* source = static_cast<const lv_image_dsc_t*>(lv_image_get_src(image));
    const auto* pixels = source->data;
    lv_image_set_rotation(image, 900);
    Publish(WidgetPropertyType::IMG_HEIGHT, 100);
    Publish(WidgetPropertyType::IMG_WIDTH, 40);
    lv_point_t pivot;
    lv_image_get_pivot(image, &pivot);
    zassert_equal(pivot.x, 20);
    zassert_equal(pivot.y, 50);
    zassert_equal(lv_obj_get_width(image), 40);
    zassert_equal(lv_obj_get_height(image), 100);
    Publish(WidgetPropertyType::ANCHOR_POINT_Y, 90);
    Publish(WidgetPropertyType::IMG_WIDTH, 20);
    lv_image_get_pivot(image, &pivot);
    zassert_equal(pivot.x, 10);
    zassert_equal(pivot.y, 10);
    Publish(WidgetPropertyType::IMG_HEIGHT, 150);
    lv_image_get_pivot(image, &pivot);
    zassert_equal(pivot.y, 60);
    zassert_equal(Inner(widget), image);
    zassert_equal(lv_image_get_src(image), source);
    zassert_equal(source->data, pixels);
    zassert_equal(lv_image_get_rotation(image), 900);
}

ZTEST(icon_lifecycle, test_demo_dial_geometry_and_image_rotation_baseline) {
    ScopedLvglLock lock;
    for(bool smoothed : { false, true }) {
        auto configuration = DialConfiguration(9);
        configuration->position_grid = { 0, 0 };
        configuration->size_grid = { 466, 466 };
        configuration->z_index = 0;
        configuration->properties[WidgetPropertyType::MIN_VALUE] = 0;
        configuration->properties[WidgetPropertyType::MAX_VALUE] = 100;
        configuration->properties[WidgetPropertyType::IS_SMOOTHED] = smoothed;
        auto needle = views_test::NeedleConfiguration(12, IconType::Image);
        needle->properties[WidgetPropertyType::POSITION_X] = 0;
        needle->properties[WidgetPropertyType::POSITION_Y] = -104;
        needle->properties[WidgetPropertyType::ANCHOR_POINT_X] = 7;
        needle->properties[WidgetPropertyType::ANCHOR_POINT_Y] = 7;
        auto context = ImageContext(*needle, 15, 220);
        auto root = std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(466, false).SetHeight(466, false).Build());
        TestDial dial(9, root, context);
        dial.SetSizePx({ 466, 466 });
        dial.Assemble(configuration, needle, context);
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
    auto configuration = DialConfiguration();
    configuration->properties[WidgetPropertyType::START_ANGLE] = 180;
    configuration->properties[WidgetPropertyType::END_ANGLE] = 450;
    auto needle = views_test::NeedleConfiguration(2, IconType::Image);
    auto context = ImageContext(*needle, 15, 220);
    TestDial dial(1, MakeRoot(), context);
    dial.Assemble(configuration, needle, context);
    zassert_equal(dial.Render(), 0);
    dial.OnActivated();
    auto* image = dial.Needle().GetIconContainer()->GetObject();
    lv_point_t pivot;
    lv_image_get_pivot(image, &pivot);
    zassert_equal(pivot.x, 7);
    zassert_equal(pivot.y, 110);
    for(int value : { 0, 50, 100 }) {
        Publish(WidgetPropertyType::VALUE, value);
        zassert_equal(dial.GetAngleForValue(value), 27 * value);
        zassert_equal(lv_image_get_rotation(image), 27 * value);
    }
}

ZTEST(icon_lifecycle, test_the_same_dial_drives_image_shape_and_label_needles) {
    ScopedLvglLock lock;
    for(auto type : { IconType::Image, IconType::TriangleIsosceles, IconType::Label }) {
        auto configuration = DialConfiguration();
        configuration->properties[WidgetPropertyType::START_ANGLE] = 180;
        configuration->properties[WidgetPropertyType::END_ANGLE] = 450;
        auto needle = views_test::NeedleConfiguration(2, type);
        needle->properties[WidgetPropertyType::ANCHOR_POINT_X] = 3;
        needle->properties[WidgetPropertyType::ANCHOR_POINT_Y] = 7;
        if(type == IconType::TriangleIsosceles) {
            needle->properties[WidgetPropertyType::WIDTH_PX] = 20;
            needle->properties[WidgetPropertyType::HEIGHT_PX] = 60;
        }
        auto context = type == IconType::Image ? ImageContext(*needle, 20, 60) : WidgetContext{};
        TestDial dial(1, MakeRoot(), context);
        dial.Assemble(configuration, needle, context);
        zassert_equal(dial.Render(), 0);
        dial.OnActivated();
        for(int value : { 0, 50, 100 }) {
            Publish(WidgetPropertyType::VALUE, value);
            if(type == IconType::Label)
                zassert_equal(lv_obj_get_style_transform_rotation(
                    views_test::WidgetContent(dial.Needle()), LV_PART_MAIN), 27 * value);
            else
                CheckRotation(dial.Needle(), 27 * value, 3, 53);
            zassert_equal(lv_obj_get_style_transform_rotation(views_test::WidgetContent(dial), LV_PART_MAIN), 0);
        }
    }
}

ZTEST(icon_lifecycle, test_dial_and_needle_keep_isolated_properties_and_bindings) {
    ScopedLvglLock lock;
    auto configuration = DialConfiguration();
    configuration->properties[WidgetPropertyType::START_ANGLE] = 180;
    configuration->properties[WidgetPropertyType::END_ANGLE] = 450;
    auto needle = views_test::NeedleConfiguration(2, IconType::TriangleIsosceles);
    needle->properties[WidgetPropertyType::WIDTH_PX] = 20;
    needle->properties[WidgetPropertyType::HEIGHT_PX] = 60;
    constexpr uint32_t needle_visibility = 1000;
    needle->bindings.push_back(PropertyBinding {
        .target = WidgetPropertyType::IS_VISIBLE,
        .channel = EventChannelId::Sensors,
        .event_type = std::to_underlying(SensorEventType::DataUpdated),
        .payload_key = std::to_underlying(SensorPayloadType::Value),
        .selector_key = std::to_underlying(SensorPayloadType::SensorId),
        .selector_value = static_cast<int>(needle_visibility)
    });
    auto set_needle_visible = [](bool visible) {
        SensorEventsChannel::GetInstance().Publish({
            .source_id = 0,
            .type = SensorEventType::DataUpdated,
            .payload = {
                { SensorPayloadType::SensorId, static_cast<int>(needle_visibility) },
                { SensorPayloadType::Value, visible }
            }
        });
    };
    TestDial dial(1, MakeRoot(), WidgetContext{});
    dial.Assemble(configuration, needle, WidgetContext{});
    zassert_equal(dial.Render(), 0);
    dial.OnActivated();
    auto& owned = dial.Needle();
    zassert_equal(owned.GetConfiguration(), needle);

    Publish(WidgetPropertyType::ANCHOR_POINT_X, 3);
    zassert_equal(lv_obj_get_style_transform_pivot_x(views_test::WidgetContent(dial), LV_PART_MAIN), 3);
    Publish(WidgetPropertyType::VALUE, 50);
    CheckRotation(owned, 1350, 10, 30);

    set_needle_visible(false);
    zassert_false(owned.IsVisible());
    zassert_true(dial.IsVisible());
    zassert_true(dial.IsProcessingEligible());
    Publish(WidgetPropertyType::VALUE, 100);
    CheckRotation(owned, 1350, 10, 30);
    set_needle_visible(true);
    CheckRotation(owned, 2700, 10, 30);
}

ZTEST(icon_lifecycle, test_screen_builds_the_demo_dial_from_a_separate_needle_definition) {
    ScopedLvglLock lock;
    auto screen_configuration = std::make_shared<ScreenConfiguration>(std::allocator_arg, std::pmr::get_default_resource());
    screen_configuration->id = 0;
    screen_configuration->type = ScreenType::Gauge;
    screen_configuration->grid = { .snap_enabled = true, .width = 466, .height = 466, .spacing_px = 0 };
    auto needle = views_test::NeedleConfiguration(12, IconType::Image);
    needle->properties[WidgetPropertyType::POSITION_X] = 0;
    needle->properties[WidgetPropertyType::POSITION_Y] = -104;
    needle->properties[WidgetPropertyType::ANCHOR_POINT_X] = 7;
    needle->properties[WidgetPropertyType::ANCHOR_POINT_Y] = 7;
    auto context = ImageContext(*needle, 15, 220);
    auto dial = DialConfiguration(9);
    dial->position_grid = { 0, 0 };
    dial->size_grid = { 466, 466 };
    dial->properties[WidgetPropertyType::MIN_VALUE] = 0;
    dial->properties[WidgetPropertyType::MAX_VALUE] = 100;
    dial->properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::pmr::vector<int> { 12 };
    // A forward reference: the needle is defined before its owner.
    screen_configuration->AddWidget(needle);
    screen_configuration->AddWidget(dial);
    ValidateScreen(*screen_configuration);
    auto root = std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(466, true).SetHeight(466, true).Build());
    eerie_leap::views::screens::Screen screen(0, root, context);
    screen.Configure(screen_configuration);

    const auto& roots = *screen.GetWidgets();
    zassert_equal(roots.size(), 1, "The needle is not a root");
    zassert_equal(roots[0]->GetConfiguration(), dial);
    const auto children = roots[0]->GetChildren();
    zassert_equal(children.size(), 1);
    zassert_equal(children[0]->GetConfiguration(), needle);
    zassert_equal(screen.Render(), 0);
    screen.OnActivated();
    lv_obj_update_layout(root->GetObject());
    zassert_equal(lv_obj_get_width(roots[0]->GetContainer()->GetObject()), 466);
    zassert_equal(lv_obj_get_height(roots[0]->GetContainer()->GetObject()), 466);
    auto* image = static_cast<const IconWidget&>(*children[0]).GetIconContainer()->GetObject();
    zassert_equal(lv_obj_get_width(image), 15);
    zassert_equal(lv_obj_get_height(image), 220);
    zassert_equal(lv_obj_get_x(image), 226, "image x=%d", lv_obj_get_x(image));
    zassert_equal(lv_obj_get_y(image), 19, "image y=%d", lv_obj_get_y(image));
    lv_point_t pivot;
    lv_image_get_pivot(image, &pivot);
    zassert_equal(pivot.x, 7);
    zassert_equal(pivot.y, 213);
    const struct { int value; int32_t angle; } cases[] = { { 0, 2250 }, { 50, 0 }, { 100, 1350 } };
    for(const auto& expected : cases) {
        Publish(WidgetPropertyType::VALUE, expected.value);
        zassert_equal(lv_image_get_rotation(image), expected.angle);
    }
}

ZTEST(icon_lifecycle, test_a_dial_without_its_needle_is_rejected_at_every_boundary) {
    ScopedLvglLock lock;
    auto error_of = [](auto&& action) -> std::string {
        try {
            action();
        } catch(const std::invalid_argument& error) {
            return error.what();
        }
        return {};
    };
    auto contains = [](const std::string& text, const char* fragment) {
        return text.find(fragment) != std::string::npos;
    };

    TestDial direct(1, MakeRoot(), WidgetContext{});
    zassert_true(contains(error_of([&] { direct.Configure(DialConfiguration()); }), "exactly 1 child"));
    zassert_true(contains(error_of([&] {
        WidgetFactory::GetInstance().CreateWidget(DialConfiguration(), MakeRoot(), WidgetContext{});
    }), "exactly 1 child"));

    TestDial conflicted(1, MakeRoot(), WidgetContext{});
    auto rotating = views_test::NeedleConfiguration(2);
    rotating->properties[WidgetPropertyType::ANIMATION_TYPE] = static_cast<int>(Animation::Type::Rotation);
    zassert_true(contains(error_of([&] { conflicted.Assemble(DialConfiguration(), rotating, WidgetContext{}); }),
        "driven rotation conflicts"));

    auto screen_configuration = std::make_shared<ScreenConfiguration>(std::allocator_arg, std::pmr::get_default_resource());
    screen_configuration->id = 0;
    screen_configuration->grid = { .snap_enabled = true, .width = 1, .height = 1, .spacing_px = 0 };
    auto dial = DialConfiguration(9);
    dial->position_grid = { 0, 0 };
    dial->size_grid = { 1, 1 };
    dial->properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::pmr::vector<int> { 12 };
    screen_configuration->AddWidget(dial);
    screen_configuration->AddWidget(views_test::NeedleConfiguration(12));
    ValidateScreen(*screen_configuration);
    eerie_leap::views::screens::Screen screen(0, MakeRoot(), WidgetContext{});
    screen.Configure(screen_configuration);
    const auto roots = screen.GetWidgets();

    auto legacy = std::make_shared<ScreenConfiguration>(std::allocator_arg, std::pmr::get_default_resource());
    legacy->id = 0;
    legacy->grid = screen_configuration->grid;
    auto legacy_dial = DialConfiguration(9);
    legacy_dial->position_grid = { 0, 0 };
    legacy_dial->size_grid = { 1, 1 };
    legacy->AddWidget(legacy_dial);
    const auto validation_error = error_of([&] { ValidateScreen(*legacy); });
    for(const auto* fragment : { "Screen ID: 0", "Widget ID: 9", "exactly 1 child", "received 0" })
        zassert_true(contains(validation_error, fragment), "Missing '%s' in '%s'", fragment, validation_error.c_str());
    // Screens trust validated input, but the dial itself still refuses to configure without a needle.
    const auto error = error_of([&] { screen.Configure(legacy); });
    zassert_true(contains(error, "exactly 1 child") && contains(error, "received 0"), "%s", error.c_str());
    zassert_equal(screen.GetWidgets(), roots);
    zassert_equal(roots->size(), 1);
    zassert_equal((*roots)[0]->GetChildren().size(), 1);
    zassert_equal(screen.GetConfiguration(), screen_configuration);
}

ZTEST(icon_lifecycle, test_an_icon_without_an_implemented_type_fails_its_render_without_throwing) {
    ScopedLvglLock lock;
    for(auto type : { IconType::None, static_cast<IconType>(99) }) {
        IconWidget widget(1, MakeRoot(), WidgetContext{});
        widget.Configure(Configuration(type));
        zassert_equal(widget.Render(), -1);
        zassert_false(widget.IsReady());
        zassert_is_null(widget.GetIconContainer());
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
            if(icon == IconType::Image)
                context = ImageContext(*configuration);
            auto widget = factory.CreateWidget(type, 1, root, context);
            const auto supported = widget->GetSupportedProperties();
            for(auto property : { WidgetPropertyType::ANIMATION_TYPE, WidgetPropertyType::IS_ANIMATION_ACTIVE,
                                 WidgetPropertyType::ANIMATION_DURATION_MS, WidgetPropertyType::ANCHOR_POINT_X,
                                 WidgetPropertyType::ANCHOR_POINT_Y })
                zassert_equal(std::count(supported.begin(), supported.end(), property), 1);
            widget->SetSizePx({ 80, 80 });
            InjectTestNeedle(*widget, *configuration);
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
    auto configuration = DialConfiguration();
    configuration->properties[WidgetPropertyType::ANIMATION_TYPE] = 1;
    configuration->properties[WidgetPropertyType::IS_ANIMATION_ACTIVE] = true;
    configuration->properties[WidgetPropertyType::START_ANGLE] = 180;
    configuration->properties[WidgetPropertyType::END_ANGLE] = 450;
    auto needle = views_test::NeedleConfiguration(2, IconType::Image);
    auto context = ImageContext(*needle);
    TestDial dial(1, MakeRoot(), context);
    dial.Assemble(configuration, needle, context);
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
    auto icon_configuration = Configuration(IconType::Image);
    icon_configuration->properties[WidgetPropertyType::ANIMATION_TYPE] = 1;
    icon_configuration->properties[WidgetPropertyType::IS_ANIMATION_ACTIVE] = true;
    auto icon_context = ImageContext(*icon_configuration);
    TestIcon independent(2, MakeRoot(), icon_context);
    independent.Configure(icon_configuration);
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
            auto configuration = DialConfiguration();
            configuration->properties[target] = target == WidgetPropertyType::OPACITY ? ConfigValue{0} : ConfigValue{false};
            configuration->properties[WidgetPropertyType::VALUE] = 50.0;
            auto needle_configuration = views_test::NeedleConfiguration(2, IconType::Image);
            auto context = ImageContext(*needle_configuration);
            TestDial dial(1, MakeRoot(), context);
            dial.Assemble(configuration, needle_configuration, context);
            zassert_equal(dial.Render(), 0);
            dial.OnActivated();
            auto& needle = dial.Needle();
            zassert_true(needle.IsActive());
            zassert_true(needle.IsVisible());
            zassert_false(needle.IsProcessingEligible());
            // The mount wraps no second owner; the needle's frame stays its only owner.
            zassert_is_null(dial.GetChildMount()->GetChild().get());
            zassert_equal(lv_obj_get_parent(needle.GetContainer()->GetObject()), dial.GetChildMount()->GetObject());
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
    auto configuration = DialConfiguration();
    configuration->properties[WidgetPropertyType::START_ANGLE] = 180;
    configuration->properties[WidgetPropertyType::END_ANGLE] = 450;
    auto needle = views_test::NeedleConfiguration(2, IconType::Image);
    auto context = ImageContext(*needle);
    TestDial dial(1, MakeRoot(), context);
    dial.Assemble(configuration, needle, context);
    zassert_is_null(dial.Needle().GetIconContainer());
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
        auto configuration = is_dial ? DialConfiguration() : Configuration(IconType::Image);
        configuration->properties[WidgetPropertyType::OPACITY] = 128;
        if(is_dial)
            configuration->properties[WidgetPropertyType::START_ANGLE] = 180;
        auto root = MakeRoot();
        auto needle = views_test::NeedleConfiguration(2, IconType::Image);
        auto context = ImageContext(is_dial ? *needle : *configuration);
        std::unique_ptr<WidgetBase> widget;
        if(is_dial) {
            auto dial = std::make_unique<TestDial>(1, root, context);
            dial->Assemble(configuration, needle, context);
            widget = std::move(dial);
        } else {
            widget = std::make_unique<TestIcon>(1, root, context);
            widget->Configure(configuration);
        }
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
