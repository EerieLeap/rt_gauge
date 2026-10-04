#include <zephyr/logging/log.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/reflection/caller_name.h"
#include "subsys/device_tree/dt_gpio.h"
#include "domain/canbus_com_domain/commands/canbus_com_logging_command.h"
#include "domain/logging_domain/event_bus/logging_events_channel.h"

#include "logging_controller.h"

namespace eerie_leap::controllers {

using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::domain::canbus_com_domain::commands;

using eerie_leap::domain::logging_domain::event_bus::LoggingEventsChannel;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventType;
using eerie_leap::domain::logging_domain::event_bus::LoggingPayloadType;
using eerie_leap::utilities::reflection::GetCallerName;

LOG_MODULE_REGISTER(logging_controller_logger);

LoggingController::LoggingController(
    std::shared_ptr<IGpio> gpio,
    std::shared_ptr<CanbusComService> canbus_com_service)
        : gpio_(std::move(gpio)),
        canbus_com_service_(std::move(canbus_com_service)),
        is_logging_in_progress_(false) {}

LoggingController::~LoggingController() {
    if(button_handler_id_ > 0)
        gpio_->RemoveChannelChangedHandler(LOGGING_BUTTON_CHANNEL, button_handler_id_);
}

int LoggingController::Initialize() {
    if(canbus_com_service_ == nullptr) {
        LOG_WRN("CANBus COM service not configured.");
        return -1;
    }

    if(!DtGpio::Get().has_value()) {
        LOG_WRN("No buttons configured.");
        return -1;
    }

    if(gpio_ == nullptr) {
        LOG_ERR("GPIO is not available.");
        return -1;
    }

    input_work_queue_thread_ = std::make_shared<WorkQueueThread>(
        INPUT_WORK_QUEUE_NAME,
        INPUT_WORK_QUEUE_STACK_SIZE,
        INPUT_WORK_QUEUE_PRIORITY,
        false,
        Mrm::GetExtPmr());
    if(!input_work_queue_thread_->Initialize()) {
        LOG_ERR("Failed to initialize the input work queue.");
        return -1;
    }

    int handler_id = gpio_->RegisterChannelChangedHandler(
        LOGGING_BUTTON_CHANNEL,
        GpioEdge::ACTIVE,
        [this](int channel, bool state) {
            ARG_UNUSED(channel);
            ARG_UNUSED(state);

            input_work_queue_thread_->Run([this]() {
                CanbusComLoggingCommand command(!is_logging_in_progress_);
                canbus_com_service_->SendCommand(
                    command,
                    [this](bool success) { LoggingStateUpdatedAck(success); });
            });
        });

    if(handler_id <= 0)
        return handler_id;

    button_handler_id_ = handler_id;

    return 0;
}

void LoggingController::LoggingStateUpdatedAck(bool success) {
    if(!success)
        return;

    is_logging_in_progress_ = !is_logging_in_progress_;
    PublishLoggingState(is_logging_in_progress_);
}

void LoggingController::PublishLoggingState(bool is_logging) {
    static constexpr auto caller = GetCallerName();

    LoggingEventsChannel::GetInstance().PublishAsync({
        .source_id = caller.hash,
        .type = LoggingEventType::StatusUpdated,
        .payload = { { LoggingPayloadType::IsActive, is_logging } }
    });
}

} // namespace eerie_leap::controllers
