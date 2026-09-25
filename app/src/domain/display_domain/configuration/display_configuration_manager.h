#pragma once

#include <memory>
#include <span>

#include "configuration/cbor/cbor_display_config/cbor_display_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "domain/configuration_domain/utilities/cbor_configuration_manager_base.h"
#include "domain/configuration_domain/utilities/i_configuration_manager.h"
#include "domain/display_domain/configuration/parsers/display_configuration_cbor_parser.h"
#include "domain/display_domain/models/display_configuration.h"

namespace eerie_leap::domain::display_domain::configuration {

namespace config_services = eerie_leap::configuration::services;
using eerie_leap::domain::configuration_domain::utilities::CborConfigurationManagerBase;
using eerie_leap::domain::configuration_domain::utilities::IConfigurationManager;

using eerie_leap::domain::display_domain::models::DisplayConfiguration;
using eerie_leap::domain::display_domain::configuration::parsers::DisplayConfigurationCborParser;

class DisplayConfigurationManager
    : public CborConfigurationManagerBase<DisplayConfiguration, CborDisplayConfig>,
      public IConfigurationManager {
private:
    DisplayConfigurationCborParser cbor_parser_;

    ConfigurationUpdatedHandler configuration_updated_handler_;

    eerie_memory::pmr_unique_ptr<CborDisplayConfig> Serialize(const DisplayConfiguration& configuration) override;
    eerie_memory::pmr_unique_ptr<DisplayConfiguration> Deserialize(const CborDisplayConfig& cbor_config) override;
    bool CreateDefaultConfiguration() override;

public:
    explicit DisplayConfigurationManager(
        std::unique_ptr<config_services::CborConfigurationService<CborDisplayConfig>> cbor_configuration_service);

    void RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) override;

    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data) override;
};

} // namespace eerie_leap::domain::display_domain::configuration
