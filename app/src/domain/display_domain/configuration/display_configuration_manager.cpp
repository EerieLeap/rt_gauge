#include <exception>

#include <zephyr/logging/log.h>

#include "utilities/memory/memory_resource_manager.h"

#include "display_configuration_manager.h"

namespace eerie_leap::domain::display_domain::configuration {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::configuration::services;

LOG_MODULE_REGISTER(display_config_ctrl_logger);

DisplayConfigurationManager::DisplayConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborDisplayConfig>> cbor_configuration_service) :
    CborConfigurationManagerBase("Display", std::move(cbor_configuration_service)) {

    LoadOrCreateDefault();
}

void DisplayConfigurationManager::RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) {
    configuration_updated_handler_ = std::move(handler);
}

bool DisplayConfigurationManager::ApplyCborConfiguration(std::span<const uint8_t> cbor_data) {
    if(!CborConfigurationManagerBase::ApplyCborConfiguration(cbor_data))
        return false;

    // Only the externally supplied configuration needs to be pushed to the
    // driver; a local Update() comes from the service that already applied it.
    if(configuration_updated_handler_) {
        try {
            configuration_updated_handler_();
        } catch(const std::exception& e) {
            LOG_ERR("Display configuration updated handler failed. %s", e.what());
        } catch(...) {
            LOG_ERR("Display configuration updated handler failed.");
        }
    }

    return true;
}

pmr_unique_ptr<CborDisplayConfig> DisplayConfigurationManager::Serialize(const DisplayConfiguration& configuration) {
    return cbor_parser_.Serialize(configuration);
}

pmr_unique_ptr<DisplayConfiguration> DisplayConfigurationManager::Deserialize(const CborDisplayConfig& cbor_config) {
    return cbor_parser_.Deserialize(Mrm::GetExtPmr(), cbor_config);
}

bool DisplayConfigurationManager::CreateDefaultConfiguration() {
    return Update(DisplayConfiguration {});
}

} // namespace eerie_leap::domain::display_domain::configuration
