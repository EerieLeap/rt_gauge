#pragma once

#include <memory>

#include "subsys/threading/work_queue_thread.h"

#include "domain/ble_domain/services/ble_service.h"
#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/sensor_domain/services/sensors_processing_service.h"

namespace eerie_leap::controllers {

using eerie_leap::subsys::threading::WorkQueueThread;

using eerie_leap::domain::ble_domain::services::BleService;
using eerie_leap::domain::configuration_domain::services::ConfigurationService;
using eerie_leap::domain::sensor_domain::services::SensorsProcessingService;

class BleController {
private:
    std::shared_ptr<ConfigurationService> configuration_service_;
    std::shared_ptr<SensorsProcessingService> sensors_processing_service_;
    std::shared_ptr<WorkQueueThread> config_work_queue_thread_;

    BleService* ble_service_ = nullptr;

public:
    BleController(
        std::shared_ptr<ConfigurationService> configuration_service,
        std::shared_ptr<SensorsProcessingService> sensors_processing_service,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread);

    int Initialize();
    int Start();
};

} // namespace eerie_leap::controllers
