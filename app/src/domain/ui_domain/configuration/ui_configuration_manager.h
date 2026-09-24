#pragma once

#include <memory>

#include "utilities/memory/memory_resource_manager.h"
#include "configuration/cbor/cbor_ui_config/cbor_ui_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "domain/configuration_domain/utilities/i_cbor_configuration_manager.h"
#include "domain/ui_domain/configuration/parsers/ui_configuration_cbor_parser.h"

#include "domain/ui_domain/models/ui_configuration.h"

namespace eerie_leap::domain::ui_domain::configuration {

namespace config_services = eerie_leap::configuration::services;
using eerie_leap::domain::configuration_domain::utilities::ICborConfigurationManager;

using eerie_leap::domain::ui_domain::models::UiConfiguration;
using eerie_leap::domain::ui_domain::configuration::parsers::UiConfigurationCborParser;
using eerie_leap::domain::ui_domain::configuration::parsers::UiConfigurationValidator;

class UiConfigurationManager : public ICborConfigurationManager {
private:
    std::unique_ptr<config_services::CborConfigurationService<CborUiConfig>> cbor_configuration_service_;
    std::unique_ptr<UiConfigurationCborParser> cbor_parser_;
    std::shared_ptr<UiConfiguration> configuration_;

    bool CreateDefaultConfiguration();

public:
    explicit UiConfigurationManager(
        std::unique_ptr<config_services::CborConfigurationService<CborUiConfig>> cbor_configuration_service,
        UiConfigurationValidator::ChildValidator validate_children = {});
    // Validates, persists, and then keeps the configuration itself; do not modify it afterwards.
    bool Update(std::shared_ptr<UiConfiguration> configuration);
    // The returned configuration is already validated; screens build from it without rechecking.
    std::shared_ptr<UiConfiguration> Get(bool force_load = false);

    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data) override;
    std::pmr::vector<uint8_t> GetCborConfiguration() override;
};

} // namespace eerie_leap::domain::ui_domain::configuration
