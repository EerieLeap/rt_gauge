#include <algorithm>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <eerie_memory.hpp>
#include <lvgl.h>
#include <zephyr/ztest.h>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/string/string_helpers.h"

#include "domain/sensor_domain/event_bus/sensor_events_channel.h"
#include "domain/ui_domain/configuration/parsers/ui_configuration_validator.h"
#include "domain/ui_domain/models/icon_type.h"
#include "domain/ui_domain/models/property_binding.h"
#include "domain/ui_domain/models/screen_configuration.h"
#include "domain/ui_domain/models/widget_configuration.h"
#include "domain/ui_domain/utilities/widget_property_validator.h"
#include "event_bus/event_channels.h"
#include "subsys/assets/asset_kind.h"
#include "subsys/event_bus/event_channel_id.h"
#include "views/widgets/property_value_conversion.h"

#include "views/screens/widget_assembly.h"
#include "views/utilitites/frame.h"
#include "views/utilitites/grid_layout.h"
#include "views/widgets/widget_factory.h"

#include "lottie_spinner_json.h"
#include "views_test_support.h"

using namespace eerie_memory;
using namespace eerie_leap::domain::ui_domain::models;
using eerie_leap::domain::ui_domain::utilities::WidgetPropertyValidator;
using namespace eerie_leap::domain::sensor_domain::event_bus;
using namespace eerie_leap::views::widgets;
using eerie_leap::domain::ui_domain::configuration::parsers::UiConfigurationValidator;
using eerie_leap::event_bus::InitializeEventChannels;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::utilities::string::StringHelpers;
using eerie_leap::views::screens::WidgetAssembly;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::utilitites::GridLayout;

namespace {

// 466 / 10: a cell is 46 px, and a widget of the whole grid is the whole display.
constexpr uint32_t GRID_CELLS = 10;
constexpr uint32_t CELL_PX = 46;
constexpr int32_t DISPLAY_PX = 466;

constexpr const char* X_SENSOR = "layout_x";
constexpr const char* Y_SENSOR = "layout_y";
constexpr const char* WIDTH_SENSOR = "layout_width";
constexpr const char* HEIGHT_SENSOR = "layout_height";

std::shared_ptr<Frame> MakeContainer() {
    return std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(100, false).SetHeight(100, false).Build());
}

std::shared_ptr<ScreenConfiguration> MakeScreen() {
    auto screen = make_shared_pmr<ScreenConfiguration>(Mrm::GetExtPmr());
    screen->id = 1;
    screen->screen_group_id = 1;
    screen->type = ScreenType::Gauge;
    screen->grid = { .snap_enabled = true, .width = GRID_CELLS, .height = GRID_CELLS, .spacing_px = 0 };

    return screen;
}

PropertyBinding SensorBinding(WidgetPropertyType target, const char* sensor_id) {
    return PropertyBinding{ .target = target,
        .event = { .channel = EventChannelId::Sensors,
            .event_type = std::to_underlying(SensorEventType::DataUpdated),
            .payload_key = std::to_underlying(SensorPayloadType::Value),
            .selector_key = std::to_underlying(SensorPayloadType::SensorId),
            .selector_value = std::pmr::string(sensor_id, Mrm::GetExtPmr()) } };
}

// A root bar at (2, 3), 2 by 2 cells, with its whole box bound to four sensors.
std::shared_ptr<ScreenConfiguration> MakeBoundScreen() {
    auto screen = MakeScreen();

    auto widget = make_shared_pmr<WidgetConfiguration>(Mrm::GetExtPmr());
    widget->type = WidgetType::IndicatorBar;
    widget->id = 1;
    widget->position_grid = { 2, 3 };
    widget->size_grid = { 2, 2 };
    widget->bindings.push_back(SensorBinding(WidgetPropertyType::LAYOUT_X, X_SENSOR));
    widget->bindings.push_back(SensorBinding(WidgetPropertyType::LAYOUT_Y, Y_SENSOR));
    widget->bindings.push_back(SensorBinding(WidgetPropertyType::LAYOUT_WIDTH, WIDTH_SENSOR));
    widget->bindings.push_back(SensorBinding(WidgetPropertyType::LAYOUT_HEIGHT, HEIGHT_SENSOR));
    screen->AddWidget(std::move(widget));

    return screen;
}

// Published synchronously so a test observes the result without waiting on the bus worker.
void PublishSensor(const char* sensor_id, float value) {
    SensorEventsChannel::GetInstance().Publish(
        { .source_id = 0,
            .type = SensorEventType::DataUpdated,
            .payload = { { SensorPayloadType::SensorId, StringHelpers::GetHash(sensor_id) },
                { SensorPayloadType::Value, value } } }
    );
}

// Assembled, rendered and activated: the state a widget is in while events flow.
struct Scene {
    std::shared_ptr<Frame> container = MakeContainer();
    std::shared_ptr<ScreenConfiguration> screen;
    WidgetAssembly::Roots roots;

    explicit Scene(std::shared_ptr<ScreenConfiguration> configuration, const WidgetContext& context = {}) :
        screen(std::move(configuration)) {
        roots = WidgetAssembly::Assemble(screen, container, context);

        for(auto& root : roots) {
            zassert_equal(root->Render(), 0);
            root->OnActivated();
        }
    }

    IWidget& Root() {
        return *roots.at(0);
    }

    // What the grid makes of a box in cells, which is what the record's would be.
    static WidgetSize SizePx(uint32_t width, uint32_t height) {
        return GridLayout::FromActiveScreen({ true, GRID_CELLS, GRID_CELLS, 0 }).ToPx({ width, height });
    }
};

bool Supports(const std::vector<WidgetPropertyType>& types, WidgetPropertyType type) {
    return std::ranges::find(types, type) != types.end();
}

void* SetUp() {
    views_test::EnsureTestDisplay();
    InitializeEventChannels();

    return nullptr;
}

} // namespace

ZTEST_SUITE(widget_layout, NULL, SetUp, NULL, views_test::CleanTestDisplay, NULL);

// The record's box is where it starts, as it always was.
ZTEST(widget_layout, test_a_bound_box_starts_where_the_record_puts_it) {
    Scene scene(MakeBoundScreen());

    const auto size = Scene::SizePx(2, 2);
    zassert_equal(scene.Root().GetSizePx().width, size.width);
    zassert_equal(scene.Root().GetSizePx().height, size.height);
    zassert_equal(scene.Root().GetPositionPx().x, 2 * static_cast<int>(CELL_PX));
    zassert_equal(
        scene.Root().GetPositionPx().y, DISPLAY_PX - 3 * static_cast<int>(CELL_PX) - static_cast<int>(size.height)
    );
}

// The point of the property: a bound X moves the very object the screen built, with no rebuild.
ZTEST(widget_layout, test_a_bound_x_moves_the_widget_in_place) {
    Scene scene(MakeBoundScreen());
    IWidget* const widget = &scene.Root();
    lv_obj_t* const object = widget->GetContainer()->GetObject();
    const auto y_before = widget->GetPositionPx().y;
    const auto size_before = widget->GetSizePx();
    const auto child_count = lv_obj_get_child_count(scene.container->GetObject());

    PublishSensor(X_SENSOR, 5.0F);
    lv_obj_update_layout(scene.container->GetObject());

    zassert_equal(widget->GetPositionPx().x, 5 * static_cast<int>(CELL_PX));
    zassert_equal(lv_obj_get_x(object), 5 * static_cast<int>(CELL_PX));
    zassert_equal(widget->GetPositionPx().y, y_before, "Y is the record's still.");
    zassert_equal(widget->GetSizePx().width, size_before.width);
    zassert_equal(widget->GetSizePx().height, size_before.height);

    zassert_equal_ptr(&scene.Root(), widget, "The widget is the one the screen built.");
    zassert_equal_ptr(widget->GetContainer()->GetObject(), object, "The LVGL object is the one the screen built.");
    zassert_equal(lv_obj_get_child_count(scene.container->GetObject()), child_count);
}

// Y counts up from the bottom, so a bigger Y is higher on the screen.
ZTEST(widget_layout, test_a_bound_y_counts_from_the_bottom) {
    Scene scene(MakeBoundScreen());
    const auto size = scene.Root().GetSizePx();

    PublishSensor(Y_SENSOR, 6.0F);

    zassert_equal(
        scene.Root().GetPositionPx().y, DISPLAY_PX - 6 * static_cast<int>(CELL_PX) - static_cast<int>(size.height)
    );
    zassert_equal(scene.Root().GetPositionPx().x, 2 * static_cast<int>(CELL_PX));
}

ZTEST(widget_layout, test_a_bound_size_resizes_the_widget_in_place) {
    Scene scene(MakeBoundScreen());
    lv_obj_t* const object = scene.Root().GetContainer()->GetObject();

    PublishSensor(WIDTH_SENSOR, 4.0F);
    PublishSensor(HEIGHT_SENSOR, 1.0F);
    lv_obj_update_layout(scene.container->GetObject());

    const auto size = Scene::SizePx(4, 1);
    zassert_equal(scene.Root().GetSizePx().width, size.width);
    zassert_equal(scene.Root().GetSizePx().height, size.height);
    zassert_equal(lv_obj_get_width(object), static_cast<int32_t>(size.width));
    zassert_equal(lv_obj_get_height(object), static_cast<int32_t>(size.height));
    zassert_equal_ptr(scene.Root().GetContainer()->GetObject(), object);

    // Y keeps counting from the bottom of the new, shorter box.
    zassert_equal(
        scene.Root().GetPositionPx().y, DISPLAY_PX - 3 * static_cast<int>(CELL_PX) - static_cast<int>(size.height)
    );
}

// The grid cannot hold a box under a cell, so the widget stays one cell however low the value goes.
ZTEST(widget_layout, test_a_size_under_one_cell_is_held_at_one_cell) {
    Scene scene(MakeBoundScreen());
    const auto one_cell = Scene::SizePx(1, 1);

    for(const float value : { 0.0F, -3.0F, 0.4F }) {
        PublishSensor(WIDTH_SENSOR, value);
        PublishSensor(HEIGHT_SENSOR, value);

        zassert_equal(scene.Root().GetSizePx().width, one_cell.width, "Width %f", static_cast<double>(value));
        zassert_equal(scene.Root().GetSizePx().height, one_cell.height, "Height %f", static_cast<double>(value));
    }

    // And it comes back out of it.
    PublishSensor(WIDTH_SENSOR, 3.0F);
    zassert_equal(scene.Root().GetSizePx().width, Scene::SizePx(3, 1).width);
}

// Past the grid the validator would refuse the record, so a bound value stops at its edge.
ZTEST(widget_layout, test_a_box_is_held_to_the_grid) {
    Scene scene(MakeBoundScreen());

    PublishSensor(WIDTH_SENSOR, 500.0F);
    PublishSensor(HEIGHT_SENSOR, 500.0F);
    zassert_equal(scene.Root().GetSizePx().width, static_cast<uint32_t>(DISPLAY_PX));
    zassert_equal(scene.Root().GetSizePx().height, static_cast<uint32_t>(DISPLAY_PX));

    PublishSensor(X_SENSOR, -4.0F);
    zassert_equal(scene.Root().GetPositionPx().x, 0);

    // A box of the whole grid has no room to be anywhere but at its start.
    PublishSensor(X_SENSOR, 500.0F);
    zassert_equal(scene.Root().GetPositionPx().x, 0);
}

// The box is the widget's own: an event on one sensor changes only the box property it is bound to.
ZTEST(widget_layout, test_each_box_property_follows_its_own_binding) {
    Scene scene(MakeBoundScreen());
    const auto size = scene.Root().GetSizePx();
    const auto x = scene.Root().GetPositionPx().x;

    PublishSensor(HEIGHT_SENSOR, 4.0F);

    zassert_equal(scene.Root().GetSizePx().width, size.width);
    zassert_equal(scene.Root().GetSizePx().height, Scene::SizePx(2, 4).height);
    zassert_equal(scene.Root().GetPositionPx().x, x);
}

// A root answers for its box; the properties are not on the unplaced widget, which has no grid.
ZTEST(widget_layout, test_only_a_root_widget_reports_the_box_properties) {
    Scene scene(MakeBoundScreen());
    const auto supported = scene.Root().GetSupportedProperties();

    for(auto type : { WidgetPropertyType::LAYOUT_X, WidgetPropertyType::LAYOUT_Y, WidgetPropertyType::LAYOUT_WIDTH,
            WidgetPropertyType::LAYOUT_HEIGHT })
        zassert_true(Supports(supported, type), "Property %u", static_cast<unsigned>(type));

    auto loose = WidgetFactory::GetInstance().CreateWidget(
        WidgetType::IndicatorBar, 7, MakeContainer(), WidgetContext{}
    );
    for(auto type : loose->GetSupportedProperties())
        zassert_false(WidgetPropertyValidator::IsLayoutProperty(type));
}

// A dial's needle fills the dial, so the dial answers for the box and the needle for none.
ZTEST(widget_layout, test_an_owned_child_has_no_box_properties) {
    auto screen = MakeScreen();

    auto dial = make_shared_pmr<WidgetConfiguration>(Mrm::GetExtPmr());
    dial->type = WidgetType::IndicatorDial;
    dial->id = 9;
    dial->size_grid = { GRID_CELLS, GRID_CELLS };
    dial->properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::pmr::vector<int>({ 12 }, Mrm::GetExtPmr());
    screen->AddWidget(std::move(dial));
    screen->AddWidget(views_test::NeedleConfiguration(12));

    Scene scene(screen);

    const auto owner = scene.Root().GetSupportedProperties();
    const auto needle = scene.Root().GetChildren()[0]->GetSupportedProperties();
    zassert_true(Supports(owner, WidgetPropertyType::LAYOUT_X));
    zassert_true(Supports(owner, WidgetPropertyType::LAYOUT_HEIGHT));
    zassert_false(Supports(needle, WidgetPropertyType::LAYOUT_X));
    zassert_false(Supports(needle, WidgetPropertyType::LAYOUT_WIDTH));
}

// With the unit's own child rules in the check, as UiController runs it.
ZTEST(widget_layout, test_a_binding_to_the_box_of_a_child_is_refused) {
    const auto validate_children = [](const WidgetConfiguration& owner, auto children) {
        WidgetFactory::GetInstance().ValidateChildren(owner, children);
    };

    auto screen = MakeScreen();

    auto dial = make_shared_pmr<WidgetConfiguration>(Mrm::GetExtPmr());
    dial->type = WidgetType::IndicatorDial;
    dial->id = 9;
    dial->size_grid = { GRID_CELLS, GRID_CELLS };
    dial->properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::pmr::vector<int>({ 12 }, Mrm::GetExtPmr());
    dial->bindings.push_back(SensorBinding(WidgetPropertyType::LAYOUT_X, X_SENSOR));
    auto needle = views_test::NeedleConfiguration(12);
    screen->AddWidget(dial);
    screen->AddWidget(needle);

    // The owner is a root: its binding is fine.
    UiConfigurationValidator::Validate(*screen, validate_children);

    needle->bindings.push_back(SensorBinding(WidgetPropertyType::LAYOUT_Y, Y_SENSOR));

    std::string message;
    try {
        UiConfigurationValidator::Validate(*screen, validate_children);
    } catch(const std::invalid_argument& error) {
        message = error.what();
    }

    zassert_true(message.contains("Widget ID: 12"), "'%s'", message.c_str());
    zassert_true(message.contains("a child fills its owner"), "'%s'", message.c_str());
}

namespace {

// A root widget of the given type at (0, 0), `cells` by `cells`, whose box size is bound to two sensors.
std::shared_ptr<ScreenConfiguration> MakeSizedScreen(WidgetType type, uint32_t cells, bool is_bound) {
    auto screen = MakeScreen();

    auto widget = make_shared_pmr<WidgetConfiguration>(Mrm::GetExtPmr());
    widget->type = type;
    widget->id = 1;
    widget->position_grid = { 0, 0 };
    widget->size_grid = { cells, cells };
    if(is_bound) {
        widget->bindings.push_back(SensorBinding(WidgetPropertyType::LAYOUT_WIDTH, WIDTH_SENSOR));
        widget->bindings.push_back(SensorBinding(WidgetPropertyType::LAYOUT_HEIGHT, HEIGHT_SENSOR));
    }
    screen->AddWidget(widget);

    return screen;
}

// The first thing a widget put in its content: the icon of an icon widget, the animation of a Lottie one.
lv_obj_t* ContentChild(const IWidget& widget) {
    lv_obj_t* content = views_test::WidgetContent(widget);
    const auto count = lv_obj_get_child_count(content);
    zassert_equal(count, 1, "The widget %u holds %u objects.", widget.GetId(), count);

    return lv_obj_get_child(content, 0);
}

void PublishSize(float cells) {
    PublishSensor(WIDTH_SENSOR, cells);
    PublishSensor(HEIGHT_SENSOR, cells);
}

} // namespace

// A box held inside the grid: Y past the edge would mirror, as the layout takes an absolute value.
ZTEST(widget_layout, test_a_bound_position_is_held_so_the_box_stays_inside_the_grid) {
    Scene scene(MakeBoundScreen());
    const auto size = scene.Root().GetSizePx();

    PublishSensor(Y_SENSOR, 8.0F);
    const auto at_the_top = scene.Root().GetPositionPx().y;
    zassert_equal(at_the_top, DISPLAY_PX - 8 * static_cast<int>(CELL_PX) - static_cast<int>(size.height));

    for(const float beyond : { 9.0F, 10.0F, 500.0F }) {
        PublishSensor(Y_SENSOR, beyond);
        zassert_equal(scene.Root().GetPositionPx().y, at_the_top, "Y %f", static_cast<double>(beyond));
    }

    PublishSensor(X_SENSOR, 8.0F);
    const auto at_the_edge = scene.Root().GetPositionPx().x;
    zassert_equal(at_the_edge, 8 * static_cast<int>(CELL_PX));
    PublishSensor(X_SENSOR, 9.0F);
    zassert_equal(scene.Root().GetPositionPx().x, at_the_edge);

    // A bigger box leaves less room for the position it already has.
    PublishSensor(WIDTH_SENSOR, 5.0F);
    zassert_equal(scene.Root().GetPositionPx().x, 5 * static_cast<int>(CELL_PX));
}

// The validator lets a record overhang the grid, and the box stays where the record put it.
ZTEST(widget_layout, test_a_box_the_record_lets_overhang_stays_where_the_record_puts_it) {
    auto screen = MakeBoundScreen();
    screen->widget_configurations[0]->position_grid = { 9, 9 };
    Scene scene(screen);

    zassert_equal(scene.Root().GetPositionPx().x, 9 * static_cast<int>(CELL_PX));

    // Another property changing replays them all, and the position is still the record's.
    PublishSensor(WIDTH_SENSOR, 2.0F);
    zassert_equal(scene.Root().GetPositionPx().x, 9 * static_cast<int>(CELL_PX));
}

// An arc icon places its icon on a circle of the box's radius, which it worked out once at the theme.
ZTEST(widget_layout, test_an_arc_icon_places_its_icon_on_the_circle_of_its_new_size) {
    const auto make = [](uint32_t cells, bool is_bound) {
        auto screen = MakeSizedScreen(WidgetType::BasicArcIcon, cells, is_bound);
        auto& properties = screen->widget_configurations[0]->properties;
        properties[WidgetPropertyType::ICON_TYPE] = static_cast<int>(IconType::Rectangle);
        properties[WidgetPropertyType::WIDTH_PX] = 12;
        properties[WidgetPropertyType::HEIGHT_PX] = 12;
        properties[WidgetPropertyType::POSITION_ANGLE] = 45.0;

        return screen;
    };

    Scene resized(make(4, true));
    Scene reference(make(8, false));
    lv_obj_update_layout(resized.container->GetObject());
    lv_obj_update_layout(reference.container->GetObject());
    lv_obj_t* const icon = ContentChild(resized.Root());
    const auto x_before = lv_obj_get_x(icon);
    const auto y_before = lv_obj_get_y(icon);

    PublishSize(8.0F);
    lv_obj_update_layout(resized.container->GetObject());

    lv_obj_t* const reference_icon = ContentChild(reference.Root());
    zassert_true(
        lv_obj_get_x(icon) != x_before || lv_obj_get_y(icon) != y_before, "The icon did not move with the radius."
    );
    zassert_equal(lv_obj_get_x(icon), lv_obj_get_x(reference_icon));
    zassert_equal(lv_obj_get_y(icon), lv_obj_get_y(reference_icon));
}

// A Lottie canvas is as large as the box, so it is drawn again at the new size.
ZTEST(widget_layout, test_a_lottie_widget_draws_its_animation_at_its_new_size) {
    auto store = views_test::MakeAssetStore("widget-layout-lottie");
    zassert_true(store->Save("spinner.json", views_test::TextBytes(views_test::LOTTIE_SPINNER_JSON)));
    const WidgetContext context{ .assets_manager = views_test::MakeAssetsManager(
                                     eerie_leap::subsys::assets::AssetKind::ANIMATIONS, std::move(store)
                                 ) };

    const auto make = [](uint32_t cells, bool is_bound) {
        auto screen = MakeSizedScreen(WidgetType::BasicLottie, cells, is_bound);
        screen->widget_configurations[0]->properties[WidgetPropertyType::FILE_PATH] = std::pmr::string("spinner.json");

        return screen;
    };

    Scene scene(make(1, true), context);
    const auto before = lv_canvas_get_draw_buf(ContentChild(scene.Root()))->header.w;
    zassert_equal(before, Scene::SizePx(1, 1).width);

    PublishSize(2.0F);

    // The animation is drawn again, into a canvas of the size of the new box.
    const auto* const buffer = lv_canvas_get_draw_buf(ContentChild(scene.Root()));
    zassert_not_null(buffer);
    zassert_equal(
        buffer->header.w, Scene::SizePx(2, 2).width, "The canvas stayed at %u.", static_cast<unsigned>(before)
    );
    zassert_equal(buffer->header.h, Scene::SizePx(2, 2).height);
}

// A slider keeps room at the ends of its track for the knob however its box is sized.
ZTEST(widget_layout, test_a_slider_follows_a_new_size_of_its_box) {
    Scene scene(MakeSizedScreen(WidgetType::ControlSlider, 4, true));
    lv_obj_t* const slider = ContentChild(scene.Root());

    PublishSize(6.0F);
    lv_obj_update_layout(scene.container->GetObject());

    const auto size = Scene::SizePx(6, 6);
    zassert_equal(scene.Root().GetSizePx().width, size.width);
    zassert_equal(lv_obj_get_width(views_test::WidgetContent(scene.Root())), static_cast<int32_t>(size.width));
    zassert_true(lv_obj_get_width(slider) > 0);
    zassert_equal(slider, ContentChild(scene.Root()), "The slider is the one the box held.");
}

// What a publisher sends is a number of cells, and anything else fills nothing.
ZTEST(widget_layout, test_a_published_value_is_coerced_to_whole_cells) {
    using eerie_leap::subsys::event_bus::EventData;
    using eerie_leap::views::widgets::CoerceToConfigValue;

    const auto coerce = [](const EventData& value) {
        return CoerceToConfigValue(value, 1, WidgetPropertyType::LAYOUT_X);
    };
    const auto as_int = [](const ConfigValue& value) {
        zassert_true(std::holds_alternative<int>(value));
        return std::get<int>(value);
    };

    zassert_equal(as_int(coerce(EventData{ 4.9999F })), 5, "Nearest, not truncated.");
    zassert_equal(as_int(coerce(EventData{ 4.4F })), 4);
    zassert_equal(as_int(coerce(EventData{ -2.6F })), -3);
    zassert_equal(as_int(coerce(EventData{ 7 })), 7);
    zassert_equal(as_int(coerce(EventData{ 3U })), 3);

    // Past the range an int holds, held at its end: no conversion of an out-of-range float.
    zassert_true(as_int(coerce(EventData{ 3.0e10F })) > 1000);
    zassert_true(as_int(coerce(EventData{ -3.0e10F })) < -1000);
    zassert_true(as_int(coerce(EventData{ 0xFFFFFFFFU })) > 1000, "An unsigned value does not wrap negative.");
    zassert_true(as_int(coerce(EventData{ 0x7FFFFFFF })) > 1000);

    zassert_true(std::holds_alternative<std::monostate>(coerce(EventData{ std::numeric_limits<float>::quiet_NaN() })));
    zassert_true(std::holds_alternative<std::monostate>(coerce(EventData{ std::numeric_limits<float>::infinity() })));
    zassert_true(std::holds_alternative<std::monostate>(coerce(EventData{ -std::numeric_limits<float>::infinity() })));
    zassert_true(std::holds_alternative<std::monostate>(coerce(EventData{ true })));
    zassert_true(std::holds_alternative<std::monostate>(coerce(EventData{ std::string("5") })));
}

// The same through a widget: a value that is no number moves nothing, and a huge one stops at the grid.
ZTEST(widget_layout, test_a_published_value_that_is_no_number_moves_nothing) {
    Scene scene(MakeBoundScreen());
    const auto x = scene.Root().GetPositionPx().x;
    const auto size = scene.Root().GetSizePx();

    PublishSensor(X_SENSOR, std::numeric_limits<float>::quiet_NaN());
    PublishSensor(X_SENSOR, std::numeric_limits<float>::infinity());
    PublishSensor(WIDTH_SENSOR, -std::numeric_limits<float>::infinity());

    zassert_equal(scene.Root().GetPositionPx().x, x);
    zassert_equal(scene.Root().GetSizePx().width, size.width);

    PublishSensor(X_SENSOR, 3.0e38F);
    zassert_equal(scene.Root().GetPositionPx().x, 8 * static_cast<int>(CELL_PX));

    PublishSensor(X_SENSOR, 4.6F);
    zassert_equal(scene.Root().GetPositionPx().x, 5 * static_cast<int>(CELL_PX));
}
