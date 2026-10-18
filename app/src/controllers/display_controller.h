#pragma once

#include <memory>

#include "subsys/fs/services/i_fs_service.h"
#include "subsys/threading/work_queue_thread.h"

#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/display_domain/configuration/display_configuration_manager.h"
#include "domain/display_domain/services/display_service.h"

namespace eerie_leap::controllers {

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
};

} // namespace eerie_leap::controllers
