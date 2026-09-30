#pragma once

#include <memory>

#include "subsys/smp/i_smp_forwarder.h"

#include "domain/ble_domain/services/ble_service.h"
#include "domain/sensor_domain/services/sensors_processing_service.h"

namespace eerie_leap::controllers {

using eerie_leap::subsys::smp::ISmpForwarder;

using eerie_leap::domain::ble_domain::services::BleService;
using eerie_leap::domain::sensor_domain::services::SensorsProcessingService;

class BleController {
private:
    std::shared_ptr<SensorsProcessingService> sensors_processing_service_;
    std::shared_ptr<ISmpForwarder> smp_forwarder_;

    BleService* ble_service_ = nullptr;

public:
    /** @param smp_forwarder Reaches the other units over CAN; nullptr without a COM channel. */
    BleController(
        std::shared_ptr<SensorsProcessingService> sensors_processing_service,
        std::shared_ptr<ISmpForwarder> smp_forwarder);

    int Initialize();
    int Start();
};

} // namespace eerie_leap::controllers
