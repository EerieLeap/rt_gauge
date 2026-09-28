#pragma once

#include <cstdint>
#include <memory>

#include "subsys/threading/work_queue_thread.h"
#include "subsys/cdmp/services/i_cdmp_network_info.h"

#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/configuration_domain/smp/config_mgmt_group.h"
#include "domain/system_domain/smp/device_mgmt_group.h"

namespace eerie_leap::controllers {

using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::subsys::cdmp::services::ICdmpNetworkInfo;

using eerie_leap::domain::configuration_domain::services::ConfigurationService;
using eerie_leap::domain::configuration_domain::smp::ConfigMgmtGroup;
using eerie_leap::domain::system_domain::smp::DeviceMgmtGroup;

/** @brief Serves the SMP groups of every unit: `config` and `device`. */
class ManagementController {
private:
    std::shared_ptr<ConfigurationService> configuration_service_;
    std::shared_ptr<WorkQueueThread> config_work_queue_thread_;
    std::shared_ptr<const ICdmpNetworkInfo> network_info_;
    uint32_t build_number_;

    std::unique_ptr<ConfigMgmtGroup> config_mgmt_group_;
    std::unique_ptr<DeviceMgmtGroup> device_mgmt_group_;

public:
    /**
     * @param configuration_service Must have every configuration manager registered already.
     * @param network_info This unit's CDMP identity; nullptr without CDMP.
     */
    ManagementController(
        std::shared_ptr<ConfigurationService> configuration_service,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread,
        std::shared_ptr<const ICdmpNetworkInfo> network_info,
        uint32_t build_number);

    int Initialize();
};

} // namespace eerie_leap::controllers
