#include <algorithm>
#include <array>
#include <cstring>
#include <eerie_memory.hpp>
#include <memory>
#include <memory_resource>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_ui_config/cbor_ui_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/sensor_domain/event_bus/sensor_event_type.h"
#include "domain/sensor_domain/event_bus/sensor_payload_type.h"
#include "domain/ui_domain/configuration/parsers/ui_configuration_cbor_parser.h"
#include "domain/ui_domain/configuration/ui_configuration_manager.h"
#include "domain/ui_domain/models/widget_property.h"

#include "subsys/device_tree/dt_fs.h"
#include "subsys/event_bus/event_binding.h"
#include "subsys/fs/services/fs_service.h"

#include "views/widgets/indicators/horizontal_chart_indicator/horizontal_chart_indicator.h"

using namespace eerie_memory;
using namespace eerie_leap::configuration::services;
using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::subsys::fs::services;
using namespace eerie_leap::domain::ui_domain::configuration;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::widgets::indicators;

using eerie_leap::domain::sensor_domain::event_bus::SensorEventType;
using eerie_leap::domain::sensor_domain::event_bus::SensorPayloadType;
using eerie_leap::subsys::event_bus::EventBinding;
using eerie_leap::subsys::event_bus::EventBindingChecker;

ZTEST_SUITE(ui_configuration_manager, NULL, NULL, NULL, NULL, NULL);

std::shared_ptr<UiConfiguration> CreateManagerTestUiConfiguration() {
    auto ui_configuration = make_shared_pmr<UiConfiguration>(Mrm::GetDefaultPmr());
    ui_configuration->active_screen_group_id = 3;

    auto screen_configuration = make_shared_pmr<ScreenConfiguration>(Mrm::GetDefaultPmr());
    screen_configuration->id = 8;
    screen_configuration->screen_group_id = 3;
    screen_configuration->type = ScreenType::Gauge;
    screen_configuration->z_index = -2;
    screen_configuration->is_visible = true;

    screen_configuration->grid.snap_enabled = true;
    screen_configuration->grid.width = 3;
    screen_configuration->grid.height = 3;
    screen_configuration->grid.spacing_px = 0;

    // First widget
    auto widget1 = make_shared_pmr<WidgetConfiguration>(Mrm::GetDefaultPmr());
    widget1->type = WidgetType::IndicatorArcFill;
    widget1->id = 0;
    widget1->position_grid.x = 0;
    widget1->position_grid.y = 0;
    widget1->size_grid.width = 3;
    widget1->size_grid.height = 3;
    widget1->z_index = -1;
    widget1->properties[WidgetPropertyType::MIN_VALUE] = 0;
    widget1->properties[WidgetPropertyType::MAX_VALUE] = 100;
    widget1->properties[WidgetPropertyType::LABEL] = "2348664336";

    PropertyBinding value_binding;
    value_binding.target = WidgetPropertyType::VALUE;
    value_binding.event.channel = EventChannelId::Sensors;
    value_binding.event.event_type = 0;
    value_binding.event.payload_key = 1;
    value_binding.event.selector_key = 0;
    value_binding.event.selector_value = std::pmr::string("2348664336");
    widget1->bindings.push_back(std::move(value_binding));

    PropertyBinding visibility_binding;
    visibility_binding.target = WidgetPropertyType::IS_VISIBLE;
    visibility_binding.event.channel = EventChannelId::Sensors;
    visibility_binding.event.event_type = 0;
    visibility_binding.event.payload_key = 1;
    visibility_binding.event.selector_key = 0;
    visibility_binding.event.selector_value = std::pmr::string("2348664336");
    widget1->bindings.push_back(std::move(visibility_binding));

    screen_configuration->AddWidget(std::move(widget1));

    // Second widget
    auto widget2 = make_shared_pmr<WidgetConfiguration>(Mrm::GetDefaultPmr());
    widget2->type = WidgetType::IndicatorDigital;
    widget2->id = 1;
    widget2->position_grid.x = 1;
    widget2->position_grid.y = 1;
    widget2->size_grid.width = 1;
    widget2->size_grid.height = 1;
    widget2->properties[WidgetPropertyType::IS_VISIBLE] = false;
    widget2->properties[WidgetPropertyType::MIN_VALUE] = 0;
    widget2->properties[WidgetPropertyType::MAX_VALUE] = 100;
    widget2->properties[WidgetPropertyType::LABEL] = "2348664336";

    PropertyBinding setting_binding;
    setting_binding.target = WidgetPropertyType::VALUE;
    setting_binding.event.channel = EventChannelId::Settings;
    setting_binding.event.event_type = 0;
    setting_binding.event.payload_key = 1;
    setting_binding.direction = PropertyBindingDirection::InOut;
    setting_binding.outbound_event_type = 2;
    widget2->bindings.push_back(std::move(setting_binding));

    screen_configuration->AddWidget(std::move(widget2));

    // Third widget
    auto widget3 = make_shared_pmr<WidgetConfiguration>(Mrm::GetDefaultPmr());
    widget3->type = WidgetType::IndicatorHorizontalChart;
    widget3->id = 2;
    widget3->position_grid.x = 0;
    widget3->position_grid.y = 0;
    widget3->size_grid.width = 3;
    widget3->size_grid.height = 1;
    widget3->z_index = 2;
    widget3->properties[WidgetPropertyType::MIN_VALUE] = 0;
    widget3->properties[WidgetPropertyType::MAX_VALUE] = 100;
    widget3->properties[WidgetPropertyType::LABEL] = "2348664336";
    widget3->properties[WidgetPropertyType::CHART_POINT_COUNT] = 35;
    widget3->properties[WidgetPropertyType::CHART_TYPE] = static_cast<std::uint16_t>(
        HorizontalChartIndicatorType::Line
    );
    screen_configuration->AddWidget(std::move(widget3));

    ui_configuration->screen_configurations.push_back(std::move(screen_configuration));

    return ui_configuration;
}

ZTEST(ui_configuration_manager, test_UiConfigurationManager_Save_config_successfully_saved) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto cbor_ui_configuration_service = std::make_unique<CborConfigurationService<CborUiConfig>>(
        "ui_config", fs_service
    );
    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(std::move(cbor_ui_configuration_service));

    auto ui_configuration = CreateManagerTestUiConfiguration();
    bool result = ui_configuration_manager->Update(ui_configuration);
    zassert_true(result);
}

ZTEST(ui_configuration_manager, test_UiConfigurationManager_Save_config_and_Load) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto cbor_ui_configuration_service = std::make_unique<CborConfigurationService<CborUiConfig>>(
        "ui_config", fs_service
    );
    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(std::move(cbor_ui_configuration_service));

    auto ui_configuration = CreateManagerTestUiConfiguration();
    bool result = ui_configuration_manager->Update(ui_configuration);
    zassert_true(result);

    auto saved_ui_configuration = ui_configuration_manager->Get(true);

    zassert_equal(saved_ui_configuration->active_screen_group_id, ui_configuration->active_screen_group_id);

    for(std::size_t i = 0; i < ui_configuration->screen_configurations.size(); i++) {
        zassert_equal(
            saved_ui_configuration->screen_configurations[i]->id, ui_configuration->screen_configurations[i]->id
        );
        zassert_equal(
            saved_ui_configuration->screen_configurations[i]->screen_group_id,
            ui_configuration->screen_configurations[i]->screen_group_id
        );
        zassert_equal(
            saved_ui_configuration->screen_configurations[i]->type, ui_configuration->screen_configurations[i]->type
        );
        zassert_equal(
            saved_ui_configuration->screen_configurations[i]->z_index,
            ui_configuration->screen_configurations[i]->z_index
        );
        zassert_equal(
            saved_ui_configuration->screen_configurations[i]->is_visible,
            ui_configuration->screen_configurations[i]->is_visible
        );
        zassert_equal(
            saved_ui_configuration->screen_configurations[i]->grid.snap_enabled,
            ui_configuration->screen_configurations[i]->grid.snap_enabled
        );
        zassert_equal(
            saved_ui_configuration->screen_configurations[i]->grid.width,
            ui_configuration->screen_configurations[i]->grid.width
        );
        zassert_equal(
            saved_ui_configuration->screen_configurations[i]->grid.height,
            ui_configuration->screen_configurations[i]->grid.height
        );
        zassert_equal(
            saved_ui_configuration->screen_configurations[i]->grid.spacing_px,
            ui_configuration->screen_configurations[i]->grid.spacing_px
        );

        for(std::size_t j = 0; j < ui_configuration->screen_configurations[i]->widget_configurations.size(); j++) {
            zassert_equal(
                saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->type,
                ui_configuration->screen_configurations[i]->widget_configurations[j]->type
            );
            zassert_equal(
                saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->id,
                ui_configuration->screen_configurations[i]->widget_configurations[j]->id
            );
            zassert_equal(
                saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->position_grid.x,
                ui_configuration->screen_configurations[i]->widget_configurations[j]->position_grid.x
            );
            zassert_equal(
                saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->position_grid.y,
                ui_configuration->screen_configurations[i]->widget_configurations[j]->position_grid.y
            );
            zassert_equal(
                saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->size_grid.width,
                ui_configuration->screen_configurations[i]->widget_configurations[j]->size_grid.width
            );
            zassert_equal(
                saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->size_grid.height,
                ui_configuration->screen_configurations[i]->widget_configurations[j]->size_grid.height
            );
            zassert_equal(
                saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->z_index,
                ui_configuration->screen_configurations[i]->widget_configurations[j]->z_index
            );
            zassert_equal(
                saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->properties.size(),
                ui_configuration->screen_configurations[i]->widget_configurations[j]->properties.size()
            );
            for(auto& property : ui_configuration->screen_configurations[i]->widget_configurations[j]->properties) {
                zassert_true(
                    saved_ui_configuration->screen_configurations[i]
                        ->widget_configurations[j]
                        ->properties[property.first]
                    == ui_configuration->screen_configurations[i]->widget_configurations[j]->properties[property.first]
                );
            }

            const auto& bindings = ui_configuration->screen_configurations[i]->widget_configurations[j]->bindings;
            const auto& saved_bindings =
                saved_ui_configuration->screen_configurations[i]->widget_configurations[j]->bindings;

            zassert_equal(saved_bindings.size(), bindings.size());

            for(std::size_t k = 0; k < bindings.size(); k++) {
                zassert_equal(saved_bindings[k].target, bindings[k].target);
                zassert_equal(saved_bindings[k].event.channel, bindings[k].event.channel);
                zassert_equal(saved_bindings[k].event.event_type, bindings[k].event.event_type);
                zassert_equal(saved_bindings[k].event.payload_key, bindings[k].event.payload_key);
                zassert_equal(saved_bindings[k].direction, bindings[k].direction);
                zassert_equal(saved_bindings[k].outbound_event_type, bindings[k].outbound_event_type);
                zassert_equal(saved_bindings[k].event.selector_key, bindings[k].event.selector_key);
                zassert_equal(saved_bindings[k].event.HasSelector(), bindings[k].event.HasSelector());
                zassert_true(saved_bindings[k].event.selector_value == bindings[k].event.selector_value);
            }
        }
    }
}

ZTEST(ui_configuration_manager, test_UiConfigurationManager_validates_once_per_boundary) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    size_t checks = 0;
    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(
        std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service), [&](const auto&, auto) {
            ++checks;
        }
    );
    auto ui_configuration = CreateManagerTestUiConfiguration();
    const auto widget_count = ui_configuration->screen_configurations[0]->widget_configurations.size();

    checks = 0;
    zassert_true(ui_configuration_manager->Update(ui_configuration));
    zassert_equal(checks, widget_count, "Saving validates once and keeps the configuration");
    zassert_equal(ui_configuration_manager->Get().get(), ui_configuration.get());

    const auto exported = ui_configuration_manager->GetCborConfiguration();
    checks = 0;
    zassert_true(ui_configuration_manager->ApplyCborConfiguration(exported));
    zassert_equal(checks, widget_count, "Importing validates once, while decoding");
    zassert_not_equal(ui_configuration_manager->Get().get(), ui_configuration.get());

    checks = 0;
    zassert_not_null(ui_configuration_manager->Get(true).get());
    zassert_equal(checks, widget_count, "Loading from storage validates once");
}

namespace {

std::shared_ptr<FsService> FormattedFs() {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    fs_service->Format();
    return fs_service;
}

const std::pmr::vector<int>* ChildIds(const WidgetConfiguration& widget) {
    const auto it = widget.properties.find(WidgetPropertyType::CHILD_WIDGET_IDS);
    return it != widget.properties.end() ? std::get_if<std::pmr::vector<int>>(&it->second) : nullptr;
}

} // namespace

ZTEST(ui_configuration_manager, test_UiConfigurationManager_persists_ordered_child_ids_through_storage) {
    auto fs_service = FormattedFs();
    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(
        std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service)
    );
    auto ui_configuration = CreateManagerTestUiConfiguration();
    auto& widgets = ui_configuration->screen_configurations[0]->widget_configurations;
    // Widget 2 (z 2) comes before widget 1 (z 0), and ID order, in the owner's list.
    widgets[0]->properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::pmr::vector<int>(
        { 2, 1 }, Mrm::GetDefaultPmr()
    );
    widgets[1]->properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::pmr::vector<int>(Mrm::GetDefaultPmr());
    widgets[2]->properties[WidgetPropertyType::ANCHOR_POINT_X] = 7;
    widgets[2]->properties[WidgetPropertyType::ANCHOR_POINT_Y] = 7;
    zassert_true(ui_configuration_manager->Update(ui_configuration));

    auto loaded = ui_configuration_manager->Get(true);
    zassert_not_equal(loaded.get(), ui_configuration.get(), "Reloaded from storage");
    const auto& loaded_widgets = loaded->screen_configurations[0]->widget_configurations;
    zassert_equal(loaded_widgets.size(), 3);
    const auto* owner_ids = ChildIds(*loaded_widgets[0]);
    zassert_not_null(owner_ids);
    zassert_true(*owner_ids == std::pmr::vector<int>({ 2, 1 }));
    const auto* empty_ids = ChildIds(*loaded_widgets[1]);
    zassert_not_null(empty_ids, "An empty child list is stored, not dropped");
    zassert_true(empty_ids->empty());
    zassert_equal(std::get<int>(loaded_widgets[2]->properties.at(WidgetPropertyType::ANCHOR_POINT_X)), 7);
    zassert_equal(std::get<int>(loaded_widgets[2]->properties.at(WidgetPropertyType::ANCHOR_POINT_Y)), 7);
}

ZTEST(ui_configuration_manager, test_UiConfigurationManager_rejects_invalid_compositions_before_persisting) {
    auto fs_service = FormattedFs();
    bool reject_children = false;
    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(
        std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service),
        [&](const auto&, auto children) {
            if(reject_children && !children.empty())
                throw std::invalid_argument("This owner accepts no children.");
        }
    );

    auto composed = CreateManagerTestUiConfiguration();
    composed->screen_configurations[0]->widget_configurations[0]->properties[WidgetPropertyType::CHILD_WIDGET_IDS] =
        std::pmr::vector<int>({ 1 }, Mrm::GetDefaultPmr());
    zassert_true(ui_configuration_manager->Update(composed));
    const auto exported = ui_configuration_manager->GetCborConfiguration();
    auto plain = CreateManagerTestUiConfiguration();
    zassert_true(ui_configuration_manager->Update(plain));

    auto expect_plain_stored = [&](const char* mode) {
        zassert_equal(ui_configuration_manager->Get().get(), plain.get(), "%s", mode);
        auto stored = ui_configuration_manager->Get(true);
        zassert_is_null(ChildIds(*stored->screen_configurations[0]->widget_configurations[0]), "%s", mode);
        plain = std::move(stored);
    };

    reject_children = true;
    zassert_false(ui_configuration_manager->Update(composed));
    expect_plain_stored("Rejected save");
    std::array<char, 128> reason{};
    zassert_false(ui_configuration_manager->ApplyCborConfiguration(exported, reason));
    zassert_str_equal(
        reason.data(), "Invalid UI Widget configuration. Screen ID: 8, Widget ID: 0. This owner accepts no children."
    );
    expect_plain_stored("Rejected import");

    reject_children = false;
    auto missing = CreateManagerTestUiConfiguration();
    missing->screen_configurations[0]->widget_configurations[0]->properties[WidgetPropertyType::CHILD_WIDGET_IDS] =
        std::pmr::vector<int>({ 99 }, Mrm::GetDefaultPmr());
    zassert_false(ui_configuration_manager->Update(missing), "Graph checks run without widget rules too");
    expect_plain_stored("Missing child reference");
}

ZTEST(ui_configuration_manager, test_UiConfigurationManager_falls_back_when_the_stored_version_is_stale) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    // A payload from before the schema bump: structurally valid, but unreadable now.
    auto stale_config = make_unique_pmr<CborUiConfig>(Mrm::GetDefaultPmr());
    stale_config->version = UiConfigurationCborParser::CONFIGURATION_VERSION - 1;
    stale_config->active_screen_group_id = 3;

    auto writer_service = std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service);
    zassert_true(writer_service->Save(stale_config.get()));

    auto cbor_ui_configuration_service = std::make_unique<CborConfigurationService<CborUiConfig>>(
        "ui_config", fs_service
    );
    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(std::move(cbor_ui_configuration_service));

    // The default configuration rather than a crash or a half-decoded one.
    auto configuration = ui_configuration_manager->Get();

    zassert_not_null(configuration.get());
    zassert_true(configuration->screen_configurations.empty());
}

// UiController::SetupTestConfiguration() stores through Update(), which adopts the configuration it is
// given; the service reports it, so a UI configuration written to the unit stops logging (L9).
ZTEST(ui_configuration_manager, test_an_update_is_reported_to_the_configuration_service) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(
        std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service)
    );

    eerie_leap::domain::configuration_domain::services::ConfigurationService configuration_service;
    using Type = eerie_leap::domain::configuration_domain::services::ConfigurationService::Type;
    configuration_service.RegisterCborConfigurationManager(Type::Ui, ui_configuration_manager);

    std::vector<Type> reports;
    configuration_service.RegisterConfigurationStoredHandler([&reports](Type type) {
        reports.push_back(type);
    });

    zassert_true(ui_configuration_manager->Update(CreateManagerTestUiConfiguration()));

    zassert_equal(reports.size(), 1U, "A UI configuration stored after the unit started is an update");
    zassert_equal(reports[0], Type::Ui);

    configuration_service.RegisterConfigurationStoredHandler(nullptr);
}

// A client's apply is checked against the unit's channels; loading and Update() are not.
ZTEST(ui_configuration_manager, test_an_apply_is_refused_when_the_binding_checker_refuses_a_binding) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    size_t asked = 0;
    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(
        std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service), nullptr,
        [&asked](const eerie_leap::subsys::event_bus::EventBinding& binding) -> std::string_view {
            ++asked;
            return binding.channel == eerie_leap::subsys::event_bus::EventChannelId::Settings
                ? "names an event type its channel does not have."
                : std::string_view();
        }
    );

    zassert_true(ui_configuration_manager->Update(CreateManagerTestUiConfiguration()), "A save is not checked");
    zassert_equal(asked, 0U);
    const auto exported = ui_configuration_manager->GetCborConfiguration();

    std::array<char, 160> reason{};
    zassert_false(ui_configuration_manager->ApplyCborConfiguration(exported, reason));
    zassert_not_equal(asked, 0U);
    zassert_not_null(strstr(reason.data(), "names an event type its channel does not have."), "%s", reason.data());
    zassert_not_null(strstr(reason.data(), "Binding"), "%s", reason.data());
}

ZTEST(ui_configuration_manager, test_an_apply_without_a_binding_checker_is_not_checked) {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(
        std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service)
    );

    zassert_true(ui_configuration_manager->Update(CreateManagerTestUiConfiguration()));
    const auto exported = ui_configuration_manager->GetCborConfiguration();

    zassert_true(ui_configuration_manager->ApplyCborConfiguration(exported));
}

namespace {

constexpr const char* BUTTON_SENSOR_ID = "screen_button";
constexpr const char* SLIDER_SENSOR_ID = "screen_dimmer";
constexpr const char* PLAIN_SENSOR_ID = "gps_speed";
constexpr const char* MISSING_SENSOR_ID = "no_such_sensor";

// A control that drives a sensor: its press or its position goes out as InputRequested for the sensor it
// selects, and the sensor's committed value comes back.
std::shared_ptr<WidgetConfiguration> CreateSensorControl(uint32_t id, WidgetType type, const char* sensor_id) {
    auto widget = make_shared_pmr<WidgetConfiguration>(Mrm::GetDefaultPmr());
    widget->type = type;
    widget->id = id;
    widget->position_grid.x = 0;
    widget->position_grid.y = static_cast<int>(id);
    widget->size_grid.width = 3;
    widget->size_grid.height = 1;

    PropertyBinding binding;
    binding.target = WidgetPropertyType::VALUE;
    binding.event.channel = EventChannelId::Sensors;
    binding.event.event_type = std::to_underlying(SensorEventType::DataUpdated);
    binding.event.payload_key = std::to_underlying(SensorPayloadType::Value);
    binding.event.selector_key = std::to_underlying(SensorPayloadType::SensorId);
    binding.event.selector_value = std::pmr::string(sensor_id, Mrm::GetDefaultPmr());
    binding.direction = PropertyBindingDirection::InOut;
    binding.outbound_event_type = std::to_underlying(SensorEventType::InputRequested);
    widget->bindings.push_back(std::move(binding));

    return widget;
}

// A button (widget 0) and a slider (widget 1), each driving the sensor it is given.
std::shared_ptr<UiConfiguration> CreateControlsTestUiConfiguration(
    const char* button_sensor_id = BUTTON_SENSOR_ID,
    const char* slider_sensor_id = SLIDER_SENSOR_ID
) {
    auto ui_configuration = make_shared_pmr<UiConfiguration>(Mrm::GetDefaultPmr());
    ui_configuration->active_screen_group_id = 3;

    auto screen_configuration = make_shared_pmr<ScreenConfiguration>(Mrm::GetDefaultPmr());
    screen_configuration->id = 8;
    screen_configuration->screen_group_id = 3;
    screen_configuration->type = ScreenType::Gauge;
    screen_configuration->is_visible = true;
    screen_configuration->grid.snap_enabled = true;
    screen_configuration->grid.width = 3;
    screen_configuration->grid.height = 3;
    screen_configuration->grid.spacing_px = 0;

    auto button = CreateSensorControl(0, WidgetType::ControlButton, button_sensor_id);
    button->properties[WidgetPropertyType::LABEL] = "LOG";
    screen_configuration->AddWidget(std::move(button));

    auto slider = CreateSensorControl(1, WidgetType::ControlSlider, slider_sensor_id);
    slider->properties[WidgetPropertyType::MIN_VALUE] = 10;
    slider->properties[WidgetPropertyType::MAX_VALUE] = 255;
    slider->properties[WidgetPropertyType::STEP] = 5;
    screen_configuration->AddWidget(std::move(slider));

    ui_configuration->screen_configurations.push_back(std::move(screen_configuration));

    return ui_configuration;
}

// Stands in for the check main.cpp builds over the sensors configuration: the button's and the slider's sensors are
// control sensors, one more sensor is not, and no other exists. Keeps what it was asked.
struct FakeSensors {
    std::vector<EventBinding> asked;

    EventBindingChecker Checker() {
        return [this](const EventBinding& binding) -> std::string_view {
            asked.push_back(binding);

            const auto& sensor_id = std::get<std::pmr::string>(binding.selector_value);
            if(sensor_id == BUTTON_SENSOR_ID || sensor_id == SLIDER_SENSOR_ID)
                return {};

            if(sensor_id == PLAIN_SENSOR_ID)
                return "names a sensor that is not a control sensor.";

            return "names a sensor this unit does not have.";
        };
    }

    [[nodiscard]] bool WasAsked(const char* sensor_id) const {
        return std::ranges::any_of(asked, [sensor_id](const EventBinding& binding) {
            return std::get<std::pmr::string>(binding.selector_value) == sensor_id;
        });
    }
};

} // namespace

// A button drives a control indicator and a slider a control analog, whatever either sensor does with the input.
ZTEST(ui_configuration_manager, test_an_apply_accepts_the_controls_the_sensor_checker_accepts) {
    auto fs_service = FormattedFs();
    FakeSensors sensors;
    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(
        std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service)
    );
    ui_configuration_manager->SetSensorInputChecker(sensors.Checker());

    zassert_true(ui_configuration_manager->Update(CreateControlsTestUiConfiguration()), "A save is not checked");
    zassert_true(sensors.asked.empty());
    const auto exported = ui_configuration_manager->GetCborConfiguration();

    zassert_true(ui_configuration_manager->ApplyCborConfiguration(exported));

    zassert_equal(sensors.asked.size(), 2U, "Each control is asked about once");
    zassert_true(sensors.WasAsked(BUTTON_SENSOR_ID));
    zassert_true(sensors.WasAsked(SLIDER_SENSOR_ID));
    for(const auto& binding : sensors.asked) {
        zassert_equal(binding.channel, EventChannelId::Sensors);
        zassert_equal(binding.event_type, std::to_underlying(SensorEventType::InputRequested), "The outbound event");
        zassert_equal(binding.payload_key, std::to_underlying(SensorPayloadType::Value));
        zassert_equal(binding.selector_key, std::to_underlying(SensorPayloadType::SensorId));
    }
}

ZTEST(ui_configuration_manager, test_an_apply_is_refused_when_the_sensor_checker_refuses_a_control) {
    struct Case {
        const char* button_sensor_id;
        const char* slider_sensor_id;
        const char* widget;
        const char* message;
    };
    const std::array cases = {
        Case{ BUTTON_SENSOR_ID, PLAIN_SENSOR_ID, "Screen ID: 8, Widget ID: 1.",
            "Outbound binding names a sensor that is not a control sensor." },
        Case{ MISSING_SENSOR_ID, SLIDER_SENSOR_ID, "Screen ID: 8, Widget ID: 0.",
            "Outbound binding names a sensor this unit does not have." },
    };

    for(const auto& test_case : cases) {
        auto fs_service = FormattedFs();
        FakeSensors sensors;
        auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(
            std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service)
        );
        ui_configuration_manager->SetSensorInputChecker(sensors.Checker());

        zassert_true(ui_configuration_manager->Update(
            CreateControlsTestUiConfiguration(test_case.button_sensor_id, test_case.slider_sensor_id)
        ));
        const auto exported = ui_configuration_manager->GetCborConfiguration();
        auto stored = CreateManagerTestUiConfiguration();
        zassert_true(ui_configuration_manager->Update(stored));

        std::array<char, 192> reason{};
        zassert_false(ui_configuration_manager->ApplyCborConfiguration(exported, reason));
        zassert_not_null(strstr(reason.data(), test_case.widget), "%s", reason.data());
        zassert_not_null(strstr(reason.data(), test_case.message), "%s", reason.data());
        zassert_equal(ui_configuration_manager->Get().get(), stored.get(), "A refused apply stores nothing");
    }
}

// A binding that does not publish InputRequested names no sensor to drive; the binding checker has the rest.
ZTEST(ui_configuration_manager, test_an_apply_does_not_ask_the_sensor_checker_about_other_bindings) {
    auto fs_service = FormattedFs();
    FakeSensors sensors;
    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(
        std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service)
    );
    ui_configuration_manager->SetSensorInputChecker(sensors.Checker());

    zassert_true(ui_configuration_manager->Update(CreateManagerTestUiConfiguration()));
    const auto exported = ui_configuration_manager->GetCborConfiguration();

    zassert_true(ui_configuration_manager->ApplyCborConfiguration(exported));
    zassert_true(sensors.asked.empty());
}

ZTEST(ui_configuration_manager, test_an_apply_without_a_sensor_checker_is_not_checked) {
    auto fs_service = FormattedFs();
    auto ui_configuration_manager = std::make_shared<UiConfigurationManager>(
        std::make_unique<CborConfigurationService<CborUiConfig>>("ui_config", fs_service)
    );

    zassert_true(
        ui_configuration_manager->Update(CreateControlsTestUiConfiguration(MISSING_SENSOR_ID, PLAIN_SENSOR_ID))
    );
    const auto exported = ui_configuration_manager->GetCborConfiguration();

    zassert_true(ui_configuration_manager->ApplyCborConfiguration(exported));
}
