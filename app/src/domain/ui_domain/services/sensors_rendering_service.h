#pragma once

#include <memory>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/threading/work_queue_thread.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"

#include "sensors_rendering_task.hpp"

namespace eerie_leap::domain::ui_domain::services {

using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::threading::WorkQueueTaskResult;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;
using eerie_leap::domain::sensor_domain::models::SensorReading;

namespace threading = eerie_leap::subsys::threading;

class SensorsRenderingService {
private:
    static constexpr int thread_stack_size_ = CONFIG_EERIE_LEAP_SENSORS_RENDERING_SERVICE_STACK_SIZE;
    static constexpr int thread_priority_ = 6;
    std::unique_ptr<WorkQueueThread> work_queue_thread_;
    std::optional<threading::WorkQueueTask<SensorsRenderingTask>> work_queue_task_;

    static constexpr uint32_t SENSORS_REFRESH_RATE_MS = 30;

    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;

    static WorkQueueTaskResult ProcessWorkTask(SensorsRenderingTask* task);
    static void SubmitToEventBus(const SensorReading& reading);

public:
    SensorsRenderingService(
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame);
    ~SensorsRenderingService() = default;

    void Initialize();

    void Start();
    void Restart();
    void Pause();
    void Resume();
};

} // namespace eerie_leap::domain::ui_domain::services
