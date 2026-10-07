#include <span>

#include "utilities/reflection/caller_name.h"
#include "subsys/time/time_helpers.hpp"
#include "domain/sensor_domain/event_bus/sensor_events_channel.h"

#include "sensors_rendering_service.h"

namespace eerie_leap::domain::ui_domain::services {

using namespace eerie_leap::subsys::time;
using namespace eerie_leap::domain::sensor_domain::models;
using namespace eerie_leap::domain::sensor_domain::event_bus;

using eerie_leap::utilities::reflection::GetCallerName;

LOG_MODULE_REGISTER(sensors_rendering_logger);

SensorsRenderingService::SensorsRenderingService(
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame)
        : work_queue_thread_(nullptr),
        sensor_readings_frame_(std::move(sensor_readings_frame)) {};

void SensorsRenderingService::Initialize() {
    work_queue_thread_ = std::make_unique<WorkQueueThread>(
        "sensors_rendering_service",
        thread_stack_size_,
        thread_priority_);
    work_queue_thread_->Initialize();

    auto task = std::make_unique<SensorsRenderingTask>();
    task->refresh_rate_ms = K_MSEC(SENSORS_REFRESH_RATE_MS);
    task->sensor_readings_frame = sensor_readings_frame_;

    work_queue_task_ = work_queue_thread_->CreateTask(ProcessWorkTask, std::move(task));
}

WorkQueueTaskResult SensorsRenderingService::ProcessWorkTask(SensorsRenderingTask* task) {
    try {
        // Taken in one step: a separate clear would drop readings processed in between. Readings
        // that do not fit the buffer stay pending, so the loop drains them in the same tick.
        size_t count = 0;
        do {
            count = task->sensor_readings_frame->TakeProcessedReadings(task->readings);

            for(size_t i = 0; i < count; i++) {
                // A reading without a value (a raw CAN frame) has nothing to show.
                if(task->readings[i].value.has_value())
                    SubmitToEventBus(task->readings[i]);
            }
        } while(count == task->readings.size());
    } catch (const std::exception& e) {
        LOG_DBG("Failed to render sensors. Error: %s", e.what());
    }

    return {
        .reschedule = true,
        .delay = task->refresh_rate_ms
    };
}

void SensorsRenderingService::SubmitToEventBus(const SensorReading& reading) {
    static constexpr auto caller = GetCallerName();

    SensorEventsChannel::GetInstance().PublishAsync({
        .source_id = caller.hash,
        .type = SensorEventType::DataUpdated,
        .payload = {
            { SensorPayloadType::SensorId, reading.sensor_id_hash },
            { SensorPayloadType::Value, reading.value.value_or(0.0f) }
        }
    });
}

void SensorsRenderingService::Start() {
    work_queue_task_.value().Schedule();

    LOG_INF("Processing Scheduler Service started.");
}

void SensorsRenderingService::Restart() {
    Pause();
    sensor_readings_frame_->ClearReadings();
    Start();
}

void SensorsRenderingService::Pause() {
    while(work_queue_task_.value().Cancel())
        k_sleep(K_MSEC(1));

    LOG_INF("Processing Scheduler Service stopped.");
}

void SensorsRenderingService::Resume() {
    Start();
}

} // namespace eerie_leap::domain::ui_domain::services
