#include <zephyr/logging/log.h>

#include "ble_controller.h"

namespace eerie_leap::controllers {

LOG_MODULE_REGISTER(ble_controller_logger);

BleController::BleController(
    std::shared_ptr<ConfigurationService> configuration_service,
    std::shared_ptr<SensorsProcessingService> sensors_processing_service,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread)
    : configuration_service_(std::move(configuration_service)),
      sensors_processing_service_(std::move(sensors_processing_service)),
      config_work_queue_thread_(std::move(config_work_queue_thread)) {}

int BleController::Initialize() {
    ble_service_ = &BleService::Create(configuration_service_, sensors_processing_service_, config_work_queue_thread_);

    if(!ble_service_->Initialize()) {
        LOG_ERR("Failed to initialize the BLE service.");
        return -1;
    }

    return 0;
}

int BleController::Start() {
    return ble_service_->Start() ? 0 : -1;
}

} // namespace eerie_leap::controllers
