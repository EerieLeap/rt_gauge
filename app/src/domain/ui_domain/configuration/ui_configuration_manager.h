#pragma once

#include <cstdint>
#include <memory>
#include <span>

#include "configuration/cbor/cbor_ui_config/cbor_ui_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "utilities/memory/memory_resource_manager.h"

#include "domain/configuration_domain/utilities/cbor_configuration_manager_base.h"
#include "domain/configuration_domain/utilities/i_configuration_manager.h"
#include "domain/ui_domain/configuration/parsers/ui_configuration_cbor_parser.h"

#include "domain/ui_domain/models/ui_configuration.h"

namespace eerie_leap::domain::ui_domain::configuration {

namespace config_services = eerie_leap::configuration::services;
using eerie_leap::domain::configuration_domain::utilities::CborConfigurationManagerBase;
using eerie_leap::domain::configuration_domain::utilities::IConfigurationManager;

using eerie_leap::domain::ui_domain::configuration::parsers::UiConfigurationCborParser;
using eerie_leap::domain::ui_domain::configuration::parsers::UiConfigurationValidator;
using eerie_leap::domain::ui_domain::models::UiConfiguration;
using eerie_leap::subsys::event_bus::EventBindingChecker;

class UiConfigurationManager :
    public CborConfigurationManagerBase<UiConfiguration, CborUiConfig>,
    public IConfigurationManager {
private:
    UiConfigurationValidator::ChildValidator validate_children_;
    UiConfigurationCborParser cbor_parser_;
    EventBindingChecker check_binding_;
    ConfigurationUpdatedHandler configuration_updated_handler_;

    // Reports a stored configuration to the updated handler. A handler that throws is logged: the
    // configuration is stored by then, and the store must not turn into a failure a client retries.
    void NotifyUpdated();

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
    // Called after a configuration was stored by a client's apply or by Update(), on the storing thread,
    // so the screens can be rebuilt from it. Not for UpdateOwnSetting(). Set before the first store.
    void RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) override;

    using CborConfigurationManagerBase::ApplyCborConfiguration;

    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) override;

    // Checks a configuration as an upload is checked (the domain rules, the widgets' own child rules and the
    // bindings against this unit's channels) and stores nothing. Throws std::invalid_argument naming what is wrong.
    void ValidateConfiguration(const UiConfiguration& configuration) const;

    // The size the configuration has encoded as an upload carries it; 0 if it cannot be encoded. Encodes it, so
    // it costs what an upload's encoding costs.
    size_t GetEncodedSize(const UiConfiguration& configuration);

    // Validates, persists, and then keeps the configuration itself; do not modify it afterwards.
    bool Update(std::shared_ptr<UiConfiguration> configuration);
    // The returned configuration is already validated; screens build from it without rechecking.
    using CborConfigurationManagerBase::Get;
};

} // namespace eerie_leap::domain::ui_domain::configuration
