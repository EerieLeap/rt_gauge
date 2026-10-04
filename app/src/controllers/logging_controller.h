#pragma once

#include <memory>
#include <vector>
#include <unordered_map>

#include "subsys/gpio/i_gpio.h"
#include "subsys/threading/work_queue_thread.h"

#include "domain/canbus_com_domain/services/canbus_com_service.h"

namespace eerie_leap::controllers {

using eerie_leap::subsys::gpio::IGpio;
using eerie_leap::subsys::gpio::GpioEdge;
using eerie_leap::subsys::threading::WorkQueueThread;

using eerie_leap::domain::canbus_com_domain::services::CanbusComService;

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
    bool is_logging_in_progress_ = false;

    void LoggingStateUpdatedAck(bool success);
    void PublishLoggingState(bool is_logging);

public:
    LoggingController(
        std::shared_ptr<IGpio> gpio,
        std::shared_ptr<CanbusComService> canbus_com_service);
    ~LoggingController();

    int Initialize();
};

} // namespace eerie_leap::controllers
