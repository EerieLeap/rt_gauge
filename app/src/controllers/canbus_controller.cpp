#include <string>

#include <zephyr/logging/log.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"

#include "configuration/cbor/cbor_canbus_config/cbor_canbus_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "subsys/device_tree/dt_canbus.h"

#include "domain/canbus_domain/models/canbus_configuration.h"

#include "canbus_controller.h"

namespace eerie_leap::controllers {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::domain::canbus_domain::models;

namespace config_services = eerie_leap::configuration::services;

LOG_MODULE_REGISTER(canbus_controller_logger);

CanbusController::CanbusController(
    std::shared_ptr<IFsService> fs_service,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread,
    std::shared_ptr<ConfigurationService> configuration_service)
    : fs_service_(std::move(fs_service)),
      config_work_queue_thread_(std::move(config_work_queue_thread)),
      configuration_service_(std::move(configuration_service)) {}

int CanbusController::Initialize() {
    auto cbor_canbus_config_service = std::make_unique<config_services::CborConfigurationService<CborCanbusConfig>>(
        CANBUS_CONFIGURATION_NAME, fs_service_, config_work_queue_thread_);
    canbus_configuration_manager_ = std::make_shared<CanbusConfigurationManager>(
        std::move(cbor_canbus_config_service), nullptr);

    if(configuration_service_ != nullptr)
        configuration_service_->RegisterCborConfigurationManager(
            ConfigurationService::Type::Canbus, canbus_configuration_manager_);

    // TODO: For test purposes only
    SetupTestConfiguration();

    canbus_service_ = std::make_shared<CanbusService>(DtCanbus::Get, canbus_configuration_manager_);

    canbus_com_service_ = std::make_shared<CanbusComService>(canbus_service_);
    if(!canbus_com_service_->Initialize()) {
        LOG_ERR("Failed to initialize the CANBus COM service.");
        return -1;
    }

    network_mgmt_group_ = std::make_unique<NetworkMgmtGroup>(canbus_com_service_->GetNetworkInfo());
    network_mgmt_group_->Register();

    // Registered last so the test configuration above does not trigger a reconfiguration.
    canbus_configuration_manager_->RegisterConfigurationUpdatedHandler([this] { Reconfigure(); });

    return 0;
}

int CanbusController::Start() {
    return canbus_com_service_->Start() ? 0 : -1;
}

void CanbusController::RegisterDependentService(std::shared_ptr<IService> service) {
    if(service != nullptr)
        dependent_services_.push_back(std::move(service));
}

void CanbusController::Reconfigure() {
    canbus_com_service_->Stop();
    for(auto& service : dependent_services_)
        service->Stop();

    canbus_service_->Configure();

    canbus_com_service_->Start();
    for(auto& service : dependent_services_)
        service->Start();
}

void CanbusController::SetupTestConfiguration() {
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

    canbus_configuration_manager_->Update(*canbus_configuration);
}

} // namespace eerie_leap::controllers
