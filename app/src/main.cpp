#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <eerie_memory.hpp>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "utilities/dev_tools/coredump_reporter.h"
#include "utilities/dev_tools/system_info.h"
#include "utilities/memory/memory_resource_manager.h"
#include "utilities/string/string_helpers.h"

#include "subsys/assets/assets_manager.h"
#include "subsys/device_tree/dt_configurator.h"
#include "subsys/device_tree/dt_feature.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/device_tree/dt_gpio.h"

#include "subsys/event_bus/event_binding_subscription.h"
#include "subsys/event_bus/event_channel_id.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/gpio/gpio_factory.hpp"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/time/boot_elapsed_time_provider.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/time_service.h"

#include "domain/canbus_domain/models/canbus_configuration.h"
#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/logging_domain/control/canbus_com_logging_command_sender.h"
#include "domain/logging_domain/control/configuration/logging_control_configuration_manager.h"
#include "domain/logging_domain/control/configuration_update_logging_stop.h"
#include "domain/logging_domain/control/models/logging_control_configuration.h"
#include "domain/logging_domain/control/models/logging_trigger.h"
#include "domain/sensor_domain/event_bus/sensor_event_type.h"
#include "domain/sensor_domain/event_bus/sensor_payload_type.h"
#include "domain/sensor_domain/models/indicator_input_mode.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_type.h"
#include "domain/sensor_domain/models/sources/canbus_source.h"
#include "domain/sensor_domain/services/sensor_events_publisher.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/settings_domain/services/settings_persistence_service.h"

#include "event_bus/event_channels.h"

#include "controllers/ble_controller.h"
#include "controllers/canbus_controller.h"
#include "controllers/display_controller.h"
#include "controllers/logging_controller.h"
#include "controllers/management_controller.h"
#include "controllers/sensors_controller.h"
#include "controllers/system_controller.h"
#include "controllers/ui_controller.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::dev_tools;

using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::subsys::fs::services;
using namespace eerie_leap::subsys::gpio;
using namespace eerie_leap::subsys::threading;
using namespace eerie_leap::subsys::time;

using namespace eerie_leap::domain::configuration_domain::services;
using namespace eerie_leap::domain::canbus_domain::models;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::models::sources;
using namespace eerie_leap::domain::sensor_domain::services;
using namespace eerie_leap::domain::sensor_domain::utilities;
using namespace eerie_leap::domain::ui_domain::services;
using namespace eerie_leap::domain::settings_domain::services;

using namespace eerie_leap::event_bus;

using namespace eerie_leap::controllers;

using eerie_leap::domain::logging_domain::control::CanbusComLoggingCommandSender;
using eerie_leap::domain::logging_domain::control::ConfigurationUpdateLoggingStop;
using eerie_leap::domain::logging_domain::control::configuration::LoggingControlConfigurationManager;
using eerie_leap::domain::logging_domain::control::models::LoggingControlConfiguration;
using eerie_leap::domain::logging_domain::control::models::LoggingTrigger;
using eerie_leap::domain::sensor_domain::event_bus::SensorEventType;
using eerie_leap::domain::sensor_domain::event_bus::SensorPayloadType;
using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::subsys::event_bus::SelectorMatches;
using eerie_leap::subsys::event_bus::ToSelectorValue;
using eerie_leap::utilities::string::StringHelpers;

LOG_MODULE_REGISTER(main_logger);

constexpr uint32_t SLEEP_TIME_MS = 10000;

void SetupCanbusConfiguration(std::shared_ptr<CanbusConfigurationManager> canbus_configuration_manager);
void SetupTestSensors(std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager);
void SetupTestLoggingButton(std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager);
void SetupTestLoggingTrigger(
    std::shared_ptr<LoggingControlConfigurationManager> logging_control_configuration_manager,
    int gpio_channel_count
);

namespace {

// The gauge's logging button: a PHYSICAL_INDICATOR on GPIO channel 0, read on every edge (no sampling
// rate), debounced and made a switch, so each press flips it. A logging trigger follows its value.
constexpr std::string_view LOGGING_BUTTON_SENSOR_ID = "logging_button";
constexpr int LOGGING_BUTTON_GPIO_CHANNEL = 0;
constexpr uint32_t LOGGING_BUTTON_DEBOUNCE_MS = 30;

// Whether the unit has the button's GPIO channel: the devicetree lists it (the gpioc alias; the simulator
// that stands in on native_sim and QEMU reports channels but raises no interrupts), and GPIO came up with
// it (the count the sensors validator checks a channel against, 0 after a failed initialization).
bool HasLoggingButton(int gpio_channel_count) {
    const auto& gpio_specs = DtGpio::Get();

    return gpio_specs.has_value() && !gpio_specs->empty() && gpio_channel_count > LOGGING_BUTTON_GPIO_CHANNEL;
}

// Whether a trigger is the button's: Sensors / DataUpdated / Value, selected by SensorId, with a selector
// that matches the button by the subscription's own rule (a text id or its hash).
bool IsLoggingButtonTrigger(const LoggingTrigger& trigger) {
    const auto& binding = trigger.binding;
    if(binding.channel != EventChannelId::Sensors
        || binding.event_type != std::to_underlying(SensorEventType::DataUpdated)
        || binding.payload_key != std::to_underlying(SensorPayloadType::Value)
        || !binding.HasSelector()
        || binding.selector_key != std::to_underlying(SensorPayloadType::SensorId))
        return false;

    const auto selector = ToSelectorValue(binding.selector_value);

    return selector.has_value()
        && SelectorMatches(*selector, EventData(StringHelpers::GetHash(LOGGING_BUTTON_SENSOR_ID)));
}

LoggingTrigger MakeLoggingButtonTrigger() {
    LoggingTrigger trigger;
    trigger.binding.channel = EventChannelId::Sensors;
    trigger.binding.event_type = std::to_underlying(SensorEventType::DataUpdated);
    trigger.binding.payload_key = std::to_underlying(SensorPayloadType::Value);
    trigger.binding.selector_key = std::to_underlying(SensorPayloadType::SensorId);
    trigger.binding.selector_value = std::pmr::string(LOGGING_BUTTON_SENSOR_ID, Mrm::GetExtPmr());
    trigger.target_uid = 0; // Every unit that can log.

    return trigger;
}

} // namespace

int main() {
    // CoredumpReporter::PrintStoredDump();

    // Channels are inert until their bus registers them, thus this has to run before any
    // publisher does.
    InitializeEventChannels();

    DtConfigurator::Initialize(DtFeature::INTERNAL_FS | DtFeature::GPIO | DtFeature::DISPLAY | DtFeature::CANBUS);

    const auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    if(!fs_service->Initialize()) {
        LOG_ERR("Failed to initialize File System.");
        return -1;
    }

    // Shared: controllers register the asset kinds they own, and the management controller serves them.
    const auto assets_manager = std::make_shared<AssetsManager>();

    const auto rtc_provider = std::make_shared<RtcProvider>();
    const auto boot_elapsed_time_provider = std::make_shared<BootElapsedTimeProvider>();
    const auto time_service = std::make_shared<TimeService>(rtc_provider, boot_elapsed_time_provider);
    time_service->Initialize();

    const int config_work_queue_stack_size = 6144;
    const int config_work_queue_priority = 5;
    const auto config_work_queue_thread = std::make_shared<WorkQueueThread>(
        "config_work_queue", config_work_queue_stack_size, config_work_queue_priority
    );
    if(!config_work_queue_thread->Initialize()) {
        LOG_ERR("Failed to initialize the configuration work queue.");
        return -1;
    }

    const auto system_controller = std::make_shared<SystemController>(fs_service, config_work_queue_thread);
    if(system_controller->Initialize() != 0)
        LOG_ERR("Failed to initialize the system controller.");

    const auto configuration_service = std::make_shared<ConfigurationService>();

    std::shared_ptr<IGpio> gpio = GpioFactory(DtGpio::Get).Create();
    if(gpio->Initialize() != 0) {
        LOG_ERR("Failed to initialize GPIO.");
        gpio = nullptr;
    }

    // Owns the debounce between a setting settling and its owner writing flash. Constructed
    // before any owner so a change published during initialization is already observed.
    const auto settings_persistence_service = std::make_shared<SettingsPersistenceService>(config_work_queue_thread);
    if(settings_persistence_service->Initialize() != 0)
        LOG_ERR("Failed to initialize the settings persistence service.");

    const auto display_controller = std::make_shared<DisplayController>(
        fs_service, config_work_queue_thread, configuration_service
    );
    if(display_controller->Initialize() != 0) {
        LOG_ERR("Failed to initialize the display controller.");
        return -1;
    }

    const auto sensor_readings_frame = std::make_shared<SensorReadingsFrame>();

    const auto ui_controller = std::make_shared<UiController>(
        fs_service, config_work_queue_thread, configuration_service, assets_manager
    );
    if(ui_controller->Initialize() != 0) {
        LOG_ERR("Failed to initialize the UI controller.");
        return -1;
    }
    ui_controller->Start();

    // Started once the UI controller has built its screens, whose widgets subscribe as they are
    // configured, so no reading is taken before they listen. An overlay subscribes when it is
    // shown and is honoured from the next tick.
    const auto sensor_events_publisher = std::make_shared<SensorEventsPublisher>(sensor_readings_frame);
    if(!sensor_events_publisher->Initialize() || !sensor_events_publisher->Start())
        LOG_ERR("Failed to start the sensor events publisher.");

    // The gauge has no SD service, so the controllers below register no asset kind; the registry is
    // passed for uniformity with the units that do.
    const auto canbus_controller = std::make_shared<CanbusController>(
        fs_service, config_work_queue_thread, configuration_service,
        nullptr, // sd_fs_service
        assets_manager
    );
    // if(canbus_controller->Initialize(SetupCanbusConfiguration) != 0)
    if(canbus_controller->Initialize() != 0)
        LOG_ERR("Failed to initialize the CANBus controller.");
    canbus_controller->Start();

    const auto sensors_controller = std::make_shared<SensorsController>(
        fs_service, config_work_queue_thread, configuration_service, time_service, sensor_readings_frame,
        canbus_controller->GetService(), gpio,
        nullptr, // adc_manager
        nullptr, // adc_configuration_manager
        nullptr, // sd_fs_service
        assets_manager
    );
    // TODO: For test purposes only
    // if(sensors_controller->Initialize(SetupTestSensors) != 0)
    if(sensors_controller->Initialize(SetupTestLoggingButton) != 0)
        LOG_ERR("Failed to initialize the sensors controller.");

    canbus_controller->RegisterDependentService(sensors_controller->GetProcessingService());

    // Built after SensorsController::Initialize(), whose sensors it checks its triggers against, and
    // before SensorsController::Start(), so its triggers subscribe before the first reading.
    const auto logging_controller = std::make_shared<LoggingController>(
        fs_service, config_work_queue_thread, configuration_service, sensors_controller->GetConfigurationManager(),
        canbus_controller->GetComService(), sensor_events_publisher,
        nullptr,       // sd_fs_service
        nullptr,       // time_service
        nullptr,       // sensor_readings_frame
        std::string{}, // tool_id
        assets_manager
    );
    // Only when the controller could not be initialized: sends the logging stop for a stored configuration.
    std::unique_ptr<ConfigurationUpdateLoggingStop> configuration_update_logging_stop;
    // The count the sensors manager was built with, which the button's sensor and trigger agree on.
    const int gpio_channel_count = gpio != nullptr ? gpio->GetChannelCount() : 0;
    // TODO: For test purposes only
    if(logging_controller->Initialize([gpio_channel_count](auto logging_control_configuration_manager) {
           SetupTestLoggingTrigger(logging_control_configuration_manager, gpio_channel_count);
       })
        != 0) {
        LOG_WRN("Logging controller is not available.");

        // It would have told the units that log about a configuration stored here; the broadcast alone
        // still can.
        if(canbus_controller->GetComService() != nullptr) {
            configuration_update_logging_stop = std::make_unique<ConfigurationUpdateLoggingStop>(
                configuration_service, config_work_queue_thread, nullptr,
                std::make_shared<CanbusComLoggingCommandSender>(canbus_controller->GetComService())
            );
            if(!configuration_update_logging_stop->Initialize())
                configuration_update_logging_stop.reset();
        }
    }

    sensors_controller->Start();

    // Last of the boot's stores (the UI test configuration, the logging button's sensor and trigger):
    // from here on a configuration stored on this unit stops logging on every unit, and
    // ManagementController below lets other units write one.
    if(configuration_update_logging_stop != nullptr) {
        if(!configuration_update_logging_stop->Start())
            LOG_ERR("Failed to start the configuration update stop.");
    } else {
        logging_controller->Start();
    }

    // Constructed after every controller that registers a configuration manager.
    const auto canbus_com_service = canbus_controller->GetComService();
    const auto system_configuration = system_controller->GetConfigurationManager()->Get();
    const auto management_controller = std::make_shared<ManagementController>(
        configuration_service, config_work_queue_thread,
        canbus_com_service != nullptr ? canbus_com_service->GetNetworkInfo() : nullptr,
        system_configuration != nullptr ? system_configuration->build_number : 0, sensors_controller->GetCapabilities(),
        nullptr, assets_manager
    );
    if(management_controller->Initialize() != 0)
        LOG_ERR("Failed to initialize the management controller.");

    const auto ble_controller = std::make_shared<BleController>(
        sensors_controller->GetProcessingService(), sensor_readings_frame, canbus_com_service
    );
    if(ble_controller->Initialize() == 0)
        ble_controller->Start();
    else
        LOG_ERR("Failed to initialize the BLE controller.");

    // SystemInfo::PrintStackInfo();
    SystemInfo::PrintThreadIds();

    while(true) {
        // SystemInfo::PrintHeapInfo();
        // SystemInfo::PrintStackInfo();
        // SystemInfo::PrintThreadIds();
        k_msleep(SLEEP_TIME_MS);

        // TODO: TEMPORARY navigation smoke test
        // static uint32_t nav_tick = 0;
        // if(++nav_tick % 250 == 0) {
        //     auto nav = ui_controller->GetNavigationService();
        //     LOG_INF("Navigation: active group %u -> requesting next.", nav->GetActiveGroupId().value_or(0));
        //     nav->Next();
        //     k_msleep(200);
        //     LOG_INF("Navigation: active group is now %u.", nav->GetActiveGroupId().value_or(0));
        // }

        // TODO: For test purposes only
        // sensors_controller->EmulateReadings();
        // k_msleep(20);
    }

    return 0;
}

void SetupCanbusConfiguration(std::shared_ptr<CanbusConfigurationManager> canbus_configuration_manager) {
    const auto canbus_configuration = make_shared_pmr<CanbusConfiguration>(Mrm::GetExtPmr());
    canbus_configuration->com_configuration.bus_channel = 0;

    CanChannelConfiguration canbus_channel_configuration_0(std::allocator_arg, Mrm::GetExtPmr());
    canbus_channel_configuration_0.type = CanbusType::CLASSICAL_CAN;
    canbus_channel_configuration_0.is_extended_id = false;
    canbus_channel_configuration_0.bus_channel = 0;
    canbus_channel_configuration_0.bitrate = 1000000;
    // canbus_channel_configuration_0.data_bitrate = 2000000;

    // auto message_configuration_0 = make_shared_pmr<CanMessageConfiguration>(Mrm::GetExtPmr());
    // message_configuration_0->name = "EL_FRAME_0";
    // message_configuration_0->message_size = 8;
    // message_configuration_0->frame_id = 790;

    // CanSignalConfiguration signal_configuration_0(std::allocator_arg, Mrm::GetExtPmr());
    // signal_configuration_0.start_bit = 16;
    // signal_configuration_0.size_bits = 16;
    // signal_configuration_0.name = "RPM";
    // signal_configuration_0.unit = "rpm";
    // signal_configuration_0.factor = 0.1;
    // message_configuration_0->signal_configurations.emplace_back(std::move(signal_configuration_0));
    // canbus_channel_configuration_0.message_configurations.emplace_back(std::move(message_configuration_0));

    for(int i = 0; i < 10; i++) {
        const auto message_configuration = make_shared_pmr<CanMessageConfiguration>(Mrm::GetExtPmr());
        message_configuration->frame_id = 100 + i;
        message_configuration->name = "EL_FRAME_" + std::to_string(i);
        message_configuration->message_size = 8;

        CanSignalConfiguration signal_configuration(std::allocator_arg, Mrm::GetExtPmr());
        signal_configuration.start_bit = 0;
        signal_configuration.size_bits = 16;
        signal_configuration.name = "sensor_" + std::to_string(i);
        signal_configuration.unit = "km/h";
        message_configuration->signal_configurations.emplace_back(std::move(signal_configuration));

        canbus_channel_configuration_0.message_configurations.emplace_back(std::move(message_configuration));
    }

    canbus_configuration->channel_configurations.emplace(
        canbus_channel_configuration_0.bus_channel, std::move(canbus_channel_configuration_0)
    );

    canbus_configuration_manager->Update(*canbus_configuration);
}

void SetupTestSensors(std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager) {
    // Test Sensors

    // auto sensor_1 = make_shared_pmr<Sensor>(Mrm::GetExtPmr(), "sensor_1");

    // sensor_1->metadata.name = "Sensor 1";
    // sensor_1->metadata.unit = "";
    // sensor_1->metadata.description = "Test Sensor 1";

    // sensor_1->configuration.type = SensorType::CANBUS_ANALOG;
    // sensor_1->configuration.canbus_source = make_unique_pmr<CanbusSource>(Mrm::GetExtPmr(), 0, 790, "RPM");

    // std::vector<std::shared_ptr<Sensor>> sensors = {
    //     sensor_1
    // };

    std::vector<std::shared_ptr<Sensor>> sensors;

    for(int i = 0; i < 10; i++) {
        const auto sensor = make_shared_pmr<Sensor>(Mrm::GetExtPmr(), "sensor_" + std::to_string(i));

        sensor->metadata.name = "Sensor 1";
        sensor->metadata.unit = "";
        sensor->metadata.description = "Test Sensor 1";

        sensor->configuration.type = SensorType::CANBUS_ANALOG;
        sensor->configuration.canbus_source = make_unique_pmr<CanbusSource>(
            Mrm::GetExtPmr(), 0, 100 + i, "sensor_" + std::to_string(i)
        );

        sensors.push_back(sensor);
    }

    sensors_configuration_manager->Update(sensors);
}

// TODO: For test purposes only
// Adds the logging button to the stored sensors, when it is missing and the unit has its GPIO channel.
// The rest of the stored configuration stays as it is.
void SetupTestLoggingButton(std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager) {
    if(!HasLoggingButton(sensors_configuration_manager->GetGpioChannelCount()))
        return;

    std::vector<std::shared_ptr<Sensor>> sensors;
    if(const auto stored_sensors = sensors_configuration_manager->Get(); stored_sensors != nullptr)
        sensors = *stored_sensors;

    if(std::ranges::any_of(sensors, [](const auto& sensor) {
           return sensor->id == LOGGING_BUTTON_SENSOR_ID;
       }))
        return;

    // A sensors configuration stored before indicator conditioning does not decode, and the manager
    // runs on an unsaved default instead: the update below then replaces the stored file.
    const bool is_replacing_rejected = sensors_configuration_manager->IsStoredConfigurationRejected();

    auto logging_button = make_shared_pmr<Sensor>(Mrm::GetExtPmr(), LOGGING_BUTTON_SENSOR_ID);
    logging_button->metadata.name = "Logging button";
    logging_button->metadata.description = "Starts and stops logging; each press flips it.";
    logging_button->configuration.type = SensorType::PHYSICAL_INDICATOR;
    logging_button->configuration.channel = static_cast<uint32_t>(LOGGING_BUTTON_GPIO_CHANNEL);
    logging_button->configuration.debounce_ms = LOGGING_BUTTON_DEBOUNCE_MS;
    logging_button->configuration.input_mode = IndicatorInputMode::Toggle;
    sensors.push_back(std::move(logging_button));

    if(!sensors_configuration_manager->Update(sensors)) {
        LOG_ERR("Failed to add the logging button to the sensors configuration.");
        return;
    }

    LOG_INF("Logging button added to the sensors configuration.");
    if(is_replacing_rejected)
        LOG_WRN(
            "The stored sensors configuration could not be read; it was replaced by one holding only the logging "
            "button."
        );
}

// TODO: For test purposes only
// Adds the trigger that follows the logging button, when it is missing and the unit has the button's
// GPIO channel. The rest of the stored configuration stays as it is.
void SetupTestLoggingTrigger(
    std::shared_ptr<LoggingControlConfigurationManager> logging_control_configuration_manager,
    int gpio_channel_count
) {

    if(!HasLoggingButton(gpio_channel_count))
        return;

    // A file stored before the trigger's target does not decode, and the manager runs on an unsaved
    // default instead: the update below then replaces the stored file.
    const bool is_replacing_rejected = logging_control_configuration_manager->IsStoredConfigurationRejected();

    const auto logging_control_configuration = make_shared_pmr<LoggingControlConfiguration>(Mrm::GetExtPmr());
    if(const auto stored_configuration = logging_control_configuration_manager->Get(); stored_configuration != nullptr)
        logging_control_configuration->triggers = stored_configuration->triggers;

    if(std::ranges::any_of(logging_control_configuration->triggers, IsLoggingButtonTrigger))
        return;

    logging_control_configuration->triggers.push_back(MakeLoggingButtonTrigger());

    if(logging_control_configuration_manager->Update(*logging_control_configuration)) {
        LOG_INF("Logging button trigger added to the logging control configuration.");
        if(is_replacing_rejected)
            LOG_WRN(
                "The stored logging control configuration could not be read; it was replaced by one holding only the "
                "logging button trigger."
            );
    } else
        LOG_ERR("Failed to add the logging button trigger to the logging control configuration.");
}
