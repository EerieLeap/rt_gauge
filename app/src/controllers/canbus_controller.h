#pragma once

#include <memory>
#include <vector>

#include "subsys/fs/services/i_fs_service.h"
#include "subsys/threading/i_service.h"
#include "subsys/threading/work_queue_thread.h"

#include "domain/configuration_domain/services/configuration_service.h"

#include "domain/canbus_domain/configuration/canbus_configuration_manager.h"
#include "domain/canbus_domain/services/canbus_service.h"
#include "domain/canbus_com_domain/services/canbus_com_service.h"
#include "domain/canbus_com_domain/smp/network_mgmt_group.h"

namespace eerie_leap::controllers {

using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::subsys::threading::IService;
using eerie_leap::subsys::threading::WorkQueueThread;

using eerie_leap::domain::configuration_domain::services::ConfigurationService;

using eerie_leap::domain::canbus_domain::configuration::CanbusConfigurationManager;
using eerie_leap::domain::canbus_domain::services::CanbusService;
using eerie_leap::domain::canbus_com_domain::services::CanbusComService;
using eerie_leap::domain::canbus_com_domain::smp::NetworkMgmtGroup;

class CanbusController {
private:
    static constexpr const char* CANBUS_CONFIGURATION_NAME = "canbus_config";

    std::shared_ptr<IFsService> fs_service_;
    std::shared_ptr<WorkQueueThread> config_work_queue_thread_;
    std::shared_ptr<ConfigurationService> configuration_service_;

    std::shared_ptr<CanbusConfigurationManager> canbus_configuration_manager_;
    std::shared_ptr<CanbusService> canbus_service_;
    std::shared_ptr<CanbusComService> canbus_com_service_;
    std::unique_ptr<NetworkMgmtGroup> network_mgmt_group_;

    std::vector<std::shared_ptr<IService>> dependent_services_;

    void Reconfigure();

    // TODO: For test purposes only
    void SetupTestConfiguration();

public:
    CanbusController(
        std::shared_ptr<IFsService> fs_service,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread,
        std::shared_ptr<ConfigurationService> configuration_service);

    int Initialize();
    int Start();

    void RegisterDependentService(std::shared_ptr<IService> service);

    std::shared_ptr<CanbusService> GetService() const { return canbus_service_; }
    std::shared_ptr<CanbusComService> GetComService() const { return canbus_com_service_; }
};

} // namespace eerie_leap::controllers
