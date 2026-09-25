#pragma once

#include <memory>

#include "utilities/memory/memory_resource_manager.h"
#include "configuration/cbor/cbor_ui_config/cbor_ui_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "domain/configuration_domain/utilities/cbor_configuration_manager_base.h"
#include "domain/ui_domain/configuration/parsers/ui_configuration_cbor_parser.h"

#include "domain/ui_domain/models/ui_configuration.h"

namespace eerie_leap::domain::ui_domain::configuration {

namespace config_services = eerie_leap::configuration::services;
using eerie_leap::domain::configuration_domain::utilities::CborConfigurationManagerBase;

using eerie_leap::domain::ui_domain::models::UiConfiguration;
using eerie_leap::domain::ui_domain::configuration::parsers::UiConfigurationCborParser;
using eerie_leap::domain::ui_domain::configuration::parsers::UiConfigurationValidator;

class UiConfigurationManager : public CborConfigurationManagerBase<UiConfiguration, CborUiConfig> {
private:
    UiConfigurationCborParser cbor_parser_;

    eerie_memory::pmr_unique_ptr<CborUiConfig> Serialize(const UiConfiguration& configuration) override;
    eerie_memory::pmr_unique_ptr<UiConfiguration> Deserialize(const CborUiConfig& cbor_config) override;
    bool CreateDefaultConfiguration() override;

public:
    explicit UiConfigurationManager(
        std::unique_ptr<config_services::CborConfigurationService<CborUiConfig>> cbor_configuration_service,
        UiConfigurationValidator::ChildValidator validate_children = {});
    // Validates, persists, and then keeps the configuration itself; do not modify it afterwards.
    bool Update(std::shared_ptr<UiConfiguration> configuration);
    // The returned configuration is already validated; screens build from it without rechecking.
    using CborConfigurationManagerBase::Get;
};

} // namespace eerie_leap::domain::ui_domain::configuration
