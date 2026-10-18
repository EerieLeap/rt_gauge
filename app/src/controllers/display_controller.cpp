#include <cerrno>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <eerie_memory.hpp>
#include <zephyr/logging/log.h>

#include "utilities/memory/memory_resource_manager.h"

#include "configuration/cbor/cbor_display_config/cbor_display_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "display_controller.h"

namespace eerie_leap::controllers {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;

namespace config_services = eerie_leap::configuration::services;

LOG_MODULE_REGISTER(display_controller_logger);

DisplayController::DisplayController(
    std::shared_ptr<IFsService> fs_service,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread,
    std::shared_ptr<ConfigurationService> configuration_service
) :
    fs_service_(std::move(fs_service)),
    config_work_queue_thread_(std::move(config_work_queue_thread)),
    configuration_service_(std::move(configuration_service)) {}

int DisplayController::Initialize() {
    auto cbor_display_config_service = std::make_unique<config_services::CborConfigurationService<CborDisplayConfig>>(
        DISPLAY_CONFIGURATION_NAME, fs_service_, config_work_queue_thread_
    );
    display_configuration_manager_ = std::make_shared<DisplayConfigurationManager>(
        std::move(cbor_display_config_service)
    );

    if(configuration_service_ != nullptr)
        configuration_service_->RegisterCborConfigurationManager(
            ConfigurationService::Type::Display, display_configuration_manager_
        );

    display_service_ = std::make_shared<DisplayService>(display_configuration_manager_, config_work_queue_thread_);

    if(display_service_->Initialize() != 0) {
        LOG_ERR("Failed to initialize the display service.");
        return 0;
    }

    // Weak, because the service owns the manager that owns this handler.
    std::weak_ptr<DisplayService> display_service = display_service_;

    // A configuration pushed over BLE has to reach the panel without a reboot.
    display_configuration_manager_->RegisterConfigurationUpdatedHandler([display_service] {
        if(auto service = display_service.lock())
            service->Reload();
    });

    return 0;
}

std::shared_ptr<DisplayService> DisplayController::GetDisplayService() const {
    return display_service_;
}

} // namespace eerie_leap::controllers
