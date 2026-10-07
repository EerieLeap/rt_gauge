#pragma once

#include <array>
#include <memory>
#include <zephyr/kernel.h>

#include "domain/sensor_domain/models/sensor_limits.h"
#include "domain/sensor_domain/models/sensor_reading.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"

namespace eerie_leap::domain::ui_domain::services {

using eerie_leap::domain::sensor_domain::models::SensorLimits;
using eerie_leap::domain::sensor_domain::models::SensorReading;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;

struct SensorsRenderingTask {
    k_timeout_t refresh_rate_ms;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame;

    // Buffer for the readings taken each tick, so the tick itself does not allocate.
    std::array<SensorReading, SensorLimits::kMaxCount> readings;
};

} // namespace eerie_leap::domain::ui_domain::services
