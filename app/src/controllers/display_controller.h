#pragma once

#include <memory>

#include "subsys/event_bus/event_binding.h"
#include "subsys/fs/services/i_fs_service.h"
#include "subsys/threading/work_queue_thread.h"

#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/display_domain/configuration/display_configuration_manager.h"
#include "domain/display_domain/services/display_service.h"

namespace eerie_leap::controllers {

using eerie_leap::subsys::event_bus::EventBindingChecker;
using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::subsys::threading::WorkQueueThread;

using eerie_leap::domain::configuration_domain::services::ConfigurationService;

using eerie_leap::domain::display_domain::configuration::DisplayConfigurationManager;
using eerie_leap::domain::display_domain::services::DisplayService;

class DisplayController {
private:
    static constexpr const char* DISPLAY_CONFIGURATION_NAME = "display_config";

    std::shared_ptr<IFsService> fs_service_;
    std::shared_ptr<WorkQueueThread> config_work_queue_thread_;
    std::shared_ptr<ConfigurationService> configuration_service_;

    std::shared_ptr<DisplayConfigurationManager> display_configuration_manager_;
    std::shared_ptr<DisplayService> display_service_;

public:
    DisplayController(
        std::shared_ptr<IFsService> fs_service,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread,
        std::shared_ptr<ConfigurationService> configuration_service
    );

    int Initialize();

    std::shared_ptr<DisplayService> GetDisplayService() const;

    /**
     * @brief Sets the check a client's apply of the display configuration runs on the sensor the brightness
     *        follows: @p check_brightness_sensor returns why it is no analog sensor of this unit, or an empty text.
     *
     * Built after this controller, the sensors are handed over here, once Initialize() has made the configuration
     * manager. Set before a client's first apply; unset, no sensor is checked.
     */
    void SetBrightnessSensorChecker(EventBindingChecker check_brightness_sensor);
};

} // namespace eerie_leap::controllers
