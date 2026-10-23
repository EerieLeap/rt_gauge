#include <algorithm>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <lvgl.h>
#include <eerie_memory.hpp>
#include <zephyr/ztest.h>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/string/string_helpers.h"

#include "domain/sensor_domain/event_bus/sensor_events_channel.h"
#include "domain/ui_domain/configuration/parsers/ui_configuration_validator.h"
#include "domain/ui_domain/models/property_binding.h"
#include "domain/ui_domain/models/screen_configuration.h"
#include "domain/ui_domain/models/widget_configuration.h"
#include "domain/ui_domain/utilities/widget_property_validator.h"
#include "event_bus/event_channels.h"
#include "subsys/event_bus/event_channel_id.h"

#include "views/screens/widget_assembly.h"
#include "views/utilitites/frame.h"
#include "views/utilitites/grid_layout.h"
#include "views/widgets/widget_factory.h"

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

    explicit Scene(std::shared_ptr<ScreenConfiguration> configuration) :
        screen(std::move(configuration)) {
        roots = WidgetAssembly::Assemble(screen, container, WidgetContext{});

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
    zassert_equal(scene.Root().GetPositionPx().y, DISPLAY_PX - 3 * static_cast<int>(CELL_PX) - static_cast<int>(size.height));
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

    PublishSensor(X_SENSOR, 500.0F);
    zassert_equal(scene.Root().GetPositionPx().x, static_cast<int>((GRID_CELLS - 1) * CELL_PX));
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
