#include <memory>
#include <string>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/dev_tools/system_info.h"
#include "utilities/dev_tools/coredump_reporter.h"
#include "utilities/guid/guid_generator.h"

#include "subsys/device_tree/dt_configurator.h"
#include "subsys/device_tree/dt_feature.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/device_tree/dt_gpio.h"

#include "subsys/fs/services/fs_service.h"
#include "subsys/gpio/gpio_factory.hpp"
#include "subsys/threading/work_queue_thread.h"
#include "subsys/time/time_service.h"
#include "subsys/time/rtc_provider.h"
#include "subsys/time/boot_elapsed_time_provider.h"

#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/canbus_domain/models/canbus_configuration.h"
#include "domain/sensor_domain/models/sensor.h"
#include "domain/sensor_domain/models/sensor_type.h"
#include "domain/sensor_domain/models/sources/canbus_source.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"
#include "domain/settings_domain/services/settings_persistence_service.h"

#include "event_bus/event_channels.h"

#include "controllers/system_controller.h"
#include "controllers/canbus_controller.h"
#include "controllers/sensors_controller.h"
#include "controllers/ble_controller.h"
#include "controllers/logging_controller.h"
#include "controllers/management_controller.h"
#include "controllers/display_controller.h"
#include "controllers/ui_controller.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::dev_tools;
using namespace eerie_leap::utilities::guid;

using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::subsys::fs::services;
using namespace eerie_leap::subsys::gpio;
using namespace eerie_leap::subsys::threading;
using namespace eerie_leap::subsys::time;

using namespace eerie_leap::domain::configuration_domain::services;
using namespace eerie_leap::domain::canbus_domain::models;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::models::sources;
using namespace eerie_leap::domain::sensor_domain::utilities;
using namespace eerie_leap::domain::ui_domain::services;
using namespace eerie_leap::domain::settings_domain::services;

using namespace eerie_leap::event_bus;

using namespace eerie_leap::controllers;

LOG_MODULE_REGISTER(main_logger);

constexpr uint32_t SLEEP_TIME_MS = 10000;

void SetupCanbusConfiguration(std::shared_ptr<CanbusConfigurationManager> canbus_configuration_manager);
void SetupTestSensors(std::shared_ptr<SensorsConfigurationManager> sensors_configuration_manager);

int main() {
    // CoredumpReporter::PrintStoredDump();

    // Channels are inert until their bus registers them, thus this has to run before any
    // publisher does.
    InitializeEventChannels();

    DtConfigurator::Initialize(
        DtFeature::INTERNAL_FS
        | DtFeature::GPIO
        | DtFeature::DISPLAY
        | DtFeature::CANBUS);

    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    if(!fs_service->Initialize()) {
        LOG_ERR("Failed to initialize File System.");
        return -1;
    }

    auto rtc_provider = std::make_shared<RtcProvider>();
    auto boot_elapsed_time_provider = std::make_shared<BootElapsedTimeProvider>();
    auto time_service = std::make_shared<TimeService>(rtc_provider, boot_elapsed_time_provider);
    time_service->Initialize();

    auto guid_generator = std::make_shared<GuidGenerator>();

    int config_work_queue_stack_size = 6144;
    int config_work_queue_priority = 5;
    auto config_work_queue_thread = std::make_shared<WorkQueueThread>(
        "config_work_queue",
        config_work_queue_stack_size,
        config_work_queue_priority);
    if(!config_work_queue_thread->Initialize()) {
        LOG_ERR("Failed to initialize the configuration work queue.");
        return -1;
    }

    auto system_controller = std::make_shared<SystemController>(
        fs_service, config_work_queue_thread);
    if(system_controller->Initialize() != 0)
        LOG_ERR("Failed to initialize the system controller.");

    auto configuration_service = std::make_shared<ConfigurationService>();

    std::shared_ptr<IGpio> gpio = GpioFactory(DtGpio::Get).Create();
    if(gpio->Initialize() != 0) {
        LOG_ERR("Failed to initialize GPIO.");
        gpio = nullptr;
    }

    // Owns the debounce between a setting settling and its owner writing flash. Constructed
    // before any owner so a change published during initialization is already observed.
    auto settings_persistence_service = std::make_shared<SettingsPersistenceService>(config_work_queue_thread);
    if(settings_persistence_service->Initialize() != 0)
        LOG_ERR("Failed to initialize the settings persistence service.");

    auto display_controller = std::make_shared<DisplayController>(
        fs_service,
        config_work_queue_thread,
        configuration_service);
    if(display_controller->Initialize() != 0) {
        LOG_ERR("Failed to initialize the display controller.");
        return -1;
    }

    auto sensor_readings_frame = std::make_shared<SensorReadingsFrame>();

    auto ui_controller = std::make_shared<UiController>(
        fs_service,
        config_work_queue_thread,
        configuration_service,
        sensor_readings_frame);
    if(ui_controller->Initialize() != 0) {
        LOG_ERR("Failed to initialize the UI controller.");
        return -1;
    }
    ui_controller->Start();

    auto canbus_controller = std::make_shared<CanbusController>(
        fs_service,
        config_work_queue_thread,
        configuration_service);
    // if(canbus_controller->Initialize(SetupCanbusConfiguration) != 0)
    if(canbus_controller->Initialize() != 0)
        LOG_ERR("Failed to initialize the CANBus controller.");
    canbus_controller->Start();

    auto sensors_controller = std::make_shared<SensorsController>(
        fs_service,
        config_work_queue_thread,
        configuration_service,
        time_service,
        guid_generator,
        sensor_readings_frame,
        canbus_controller->GetService(),
        gpio);
    // TODO: For test purposes only
    // if(sensors_controller->Initialize(SetupTestSensors) != 0)
    if(sensors_controller->Initialize() != 0)
        LOG_ERR("Failed to initialize the sensors controller.");

    canbus_controller->RegisterDependentService(sensors_controller->GetProcessingService());

    auto logging_controller = std::make_shared<LoggingController>(
        gpio, canbus_controller->GetComService());
    if(logging_controller->Initialize() != 0)
        LOG_WRN("Logging controller is not available.");

    sensors_controller->Start();

    // Constructed after every controller that registers a configuration manager.
    const auto canbus_com_service = canbus_controller->GetComService();
    const auto system_configuration = system_controller->GetConfigurationManager()->Get();
    auto management_controller = std::make_shared<ManagementController>(
        configuration_service,
        config_work_queue_thread,
        canbus_com_service != nullptr ? canbus_com_service->GetNetworkInfo() : nullptr,
        system_configuration != nullptr ? system_configuration->build_number : 0);
    if(management_controller->Initialize() != 0)
        LOG_ERR("Failed to initialize the management controller.");

    auto ble_controller = std::make_shared<BleController>(
        sensors_controller->GetProcessingService(),
        sensor_readings_frame,
        canbus_com_service);
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
    auto canbus_configuration = make_shared_pmr<CanbusConfiguration>(Mrm::GetExtPmr());
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
        auto message_configuration = make_shared_pmr<CanMessageConfiguration>(Mrm::GetExtPmr());
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
        canbus_channel_configuration_0.bus_channel,
        std::move(canbus_channel_configuration_0));

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
        auto sensor = make_shared_pmr<Sensor>(Mrm::GetExtPmr(), "sensor_" + std::to_string(i));

        sensor->metadata.name = "Sensor 1";
        sensor->metadata.unit = "";
        sensor->metadata.description = "Test Sensor 1";

        sensor->configuration.type = SensorType::CANBUS_ANALOG;
        sensor->configuration.canbus_source = make_unique_pmr<CanbusSource>(Mrm::GetExtPmr(), 0, 100 + i, "sensor_" + std::to_string(i));

        sensors.push_back(sensor);
    }

    sensors_configuration_manager->Update(sensors);
}
