#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include <unordered_map>

#include "subsys/event_bus/i_scoped_subscription.h"
#include "subsys/gpio/i_gpio.h"
#include "subsys/threading/work_queue_thread.h"

#include "domain/canbus_com_domain/services/canbus_com_service.h"
#include "domain/canbus_com_domain/state_bridge/network_state_importer.h"
#include "domain/logging_domain/event_bus/logging_network_state.h"

namespace eerie_leap::controllers {

using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::gpio::IGpio;
using eerie_leap::subsys::gpio::GpioEdge;
using eerie_leap::subsys::threading::WorkQueueThread;

using eerie_leap::domain::canbus_com_domain::services::CanbusComService;
using eerie_leap::domain::canbus_com_domain::state_bridge::NetworkStateImporter;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventsChannel;
using eerie_leap::domain::logging_domain::event_bus::LoggingNetworkState;

class LoggingController {
private:
    static constexpr int LOGGING_BUTTON_CHANNEL = 0;
    static constexpr const char* INPUT_WORK_QUEUE_NAME = "input_work_queue";
    static constexpr int INPUT_WORK_QUEUE_STACK_SIZE = 4096;
    static constexpr int INPUT_WORK_QUEUE_PRIORITY = 5;

    std::shared_ptr<IGpio> gpio_;
    std::shared_ptr<CanbusComService> canbus_com_service_;

    std::shared_ptr<WorkQueueThread> input_work_queue_thread_;

    int button_handler_id_ = 0;
    // The status the logging units share, written on the bus thread and read by a press on the
    // input work queue.
    std::atomic<bool> is_logging_active_ = false;

    AnySubscription logging_status_subscription_;
    // Publishes the combined status of the logging units on logging_events, which the logging icon
    // and logging_status_subscription_ follow. Declared last, so it stops publishing first.
    std::unique_ptr<NetworkStateImporter<LoggingNetworkState>> logging_status_importer_;

    int InitializeLoggingStatus();
    void OnLoggingStatusUpdated(const LoggingEventsChannel::EventMessage& event);
    void SendLoggingCommand();

public:
    LoggingController(
        std::shared_ptr<IGpio> gpio,
        std::shared_ptr<CanbusComService> canbus_com_service);
    ~LoggingController();

    int Initialize();
};

} // namespace eerie_leap::controllers
