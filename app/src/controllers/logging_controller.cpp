#include <exception>
#include <variant>

#include <zephyr/logging/log.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"
#include "subsys/device_tree/dt_gpio.h"
#include "subsys/event_bus/event_channel.h"
#include "domain/canbus_com_domain/commands/canbus_com_logging_command.h"
#include "domain/logging_domain/event_bus/logging_events_channel.h"

#include "logging_controller.h"

namespace eerie_leap::controllers {

using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::subsys::device_tree;
using namespace eerie_leap::domain::canbus_com_domain::commands;

using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventType;
using eerie_leap::domain::logging_domain::event_bus::LoggingPayloadType;

LOG_MODULE_REGISTER(logging_controller_logger);

LoggingController::LoggingController(
    std::shared_ptr<IGpio> gpio,
    std::shared_ptr<CanbusComService> canbus_com_service)
        : gpio_(std::move(gpio)),
        canbus_com_service_(std::move(canbus_com_service)) {}

LoggingController::~LoggingController() {
    if(button_handler_id_ > 0)
        gpio_->RemoveChannelChangedHandler(LOGGING_BUTTON_CHANNEL, button_handler_id_);
}

int LoggingController::Initialize() {
    if(canbus_com_service_ == nullptr) {
        LOG_WRN("CANBus COM service not configured.");
        return -1;
    }

    // Before the button checks, so the logging icon also works on a unit without a logging button.
    if(InitializeLoggingStatus() != 0)
        return -1;

    // A unit without a logging button only shows the status.
    if(!DtGpio::Get().has_value()) {
        LOG_INF("No buttons configured.");
        return 0;
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

            input_work_queue_thread_->Run([this]() { SendLoggingCommand(); });
        });

    if(handler_id <= 0)
        return handler_id;

    button_handler_id_ = handler_id;

    return 0;
}

int LoggingController::InitializeLoggingStatus() {
    // Subscribed before the importer exists, so its first status cannot pass unseen.
    logging_status_subscription_ = CreateScopedSubscription(
        LoggingEventsChannel::GetInstance(),
        LoggingEventType::StatusUpdated,
        [this](const LoggingEventsChannel::EventMessage& event) { OnLoggingStatusUpdated(event); });

    if(logging_status_subscription_ == nullptr) {
        LOG_ERR("Failed to subscribe to logging status updates.");
        return -1;
    }

    // The importer needs a bus that carries logging_events: AppEventBus does, from
    // InitializeEventChannels() on.
    try {
        logging_status_importer_ = std::make_unique<NetworkStateImporter<LoggingNetworkState>>(canbus_com_service_);
    } catch(const std::exception& e) {
        LOG_ERR("Failed to create the logging status importer. %s", e.what());
        return -1;
    }

    return 0;
}

void LoggingController::OnLoggingStatusUpdated(const LoggingEventsChannel::EventMessage& event) {
    auto it = event.payload.find(LoggingPayloadType::IsActive);
    if(it == event.payload.end())
        return;

    if(const auto* is_active = std::get_if<bool>(&it->second))
        is_logging_active_ = *is_active;
}

void LoggingController::SendLoggingCommand() {
    // Stops while any unit logs and starts otherwise. The command is broadcast, so every logging
    // unit follows it, and the status they share, not the response, tells whether they did.
    const bool is_start = !is_logging_active_;
    CanbusComLoggingCommand command(is_start);
    canbus_com_service_->SendCommand(
        command,
        [is_start](bool success) {
            if(!success)
                LOG_WRN("Logging %s command failed.", is_start ? "start" : "stop");
        });
}

} // namespace eerie_leap::controllers
