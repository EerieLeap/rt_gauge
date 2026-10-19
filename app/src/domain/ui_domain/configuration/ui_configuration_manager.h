#pragma once

#include <memory>

#include "configuration/cbor/cbor_ui_config/cbor_ui_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "utilities/memory/memory_resource_manager.h"

#include "domain/configuration_domain/utilities/cbor_configuration_manager_base.h"
#include "domain/ui_domain/configuration/parsers/ui_configuration_cbor_parser.h"

#include "domain/ui_domain/models/ui_configuration.h"

namespace eerie_leap::domain::ui_domain::configuration {

namespace config_services = eerie_leap::configuration::services;
using eerie_leap::domain::configuration_domain::utilities::CborConfigurationManagerBase;

using eerie_leap::domain::ui_domain::configuration::parsers::UiConfigurationCborParser;
using eerie_leap::domain::ui_domain::configuration::parsers::UiConfigurationValidator;
using eerie_leap::domain::ui_domain::models::UiConfiguration;
using eerie_leap::subsys::event_bus::EventBindingChecker;

class UiConfigurationManager : public CborConfigurationManagerBase<UiConfiguration, CborUiConfig> {
private:
    UiConfigurationCborParser cbor_parser_;
    EventBindingChecker check_binding_;

    eerie_memory::PmrUniquePtr<CborUiConfig> Serialize(const UiConfiguration& configuration) override;
    eerie_memory::PmrUniquePtr<UiConfiguration> Deserialize(const CborUiConfig& cbor_config) override;
    bool CreateDefaultConfiguration() override;
    // A client's apply also has its widget bindings checked against this unit's channels.
    void ValidateApply(const UiConfiguration& configuration) override;
    // The base's Update(const UiConfiguration&) copies; callers use the overload that keeps the instance.
    using CborConfigurationManagerBase::Update;

public:
    explicit UiConfigurationManager(
        std::unique_ptr<config_services::CborConfigurationService<CborUiConfig>> cbor_configuration_service,
        UiConfigurationValidator::ChildValidator validate_children = {},
        EventBindingChecker check_binding = {}
    );
    // Validates, persists, and then keeps the configuration itself; do not modify it afterwards.
    bool Update(std::shared_ptr<UiConfiguration> configuration);
    // The returned configuration is already validated; screens build from it without rechecking.
    using CborConfigurationManagerBase::Get;
};

} // namespace eerie_leap::domain::ui_domain::configuration
