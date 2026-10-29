#pragma once

#include <memory>
#include <span>

#include "configuration/cbor/cbor_display_config/cbor_display_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "subsys/event_bus/event_binding.h"

#include "domain/configuration_domain/utilities/cbor_configuration_manager_base.h"
#include "domain/configuration_domain/utilities/i_configuration_manager.h"
#include "domain/display_domain/configuration/parsers/display_configuration_cbor_parser.h"
#include "domain/display_domain/models/display_configuration.h"

namespace eerie_leap::domain::display_domain::configuration {

namespace config_services = eerie_leap::configuration::services;
using eerie_leap::domain::configuration_domain::utilities::CborConfigurationManagerBase;
using eerie_leap::domain::configuration_domain::utilities::IConfigurationManager;

using eerie_leap::domain::display_domain::configuration::parsers::DisplayConfigurationCborParser;
using eerie_leap::domain::display_domain::models::DisplayConfiguration;
using eerie_leap::subsys::event_bus::EventBindingChecker;

class DisplayConfigurationManager :
    public CborConfigurationManagerBase<DisplayConfiguration, CborDisplayConfig>,
    public IConfigurationManager {
private:
    ConfigurationUpdatedHandler configuration_updated_handler_;
    EventBindingChecker check_brightness_sensor_;

    eerie_memory::PmrUniquePtr<CborDisplayConfig> Serialize(const DisplayConfiguration& configuration) override;
    eerie_memory::PmrUniquePtr<DisplayConfiguration> Deserialize(const CborDisplayConfig& cbor_config) override;
    bool CreateDefaultConfiguration() override;
    // A client's apply also has the sensor the brightness follows checked against this unit's sensors.
    void ValidateApply(const DisplayConfiguration& configuration) override;

public:
    explicit DisplayConfigurationManager(
        std::unique_ptr<config_services::CborConfigurationService<CborDisplayConfig>> cbor_configuration_service
    );

    void RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) override;

    // Checks the sensor the brightness follows: @p check_brightness_sensor is given the binding to the sensor's
    // committed value and returns why the sensor is no analog sensor of this unit, or an empty text. The sensors
    // controller is built after the display controller, so this comes after the manager. Set before a client's
    // first apply; unset, no sensor is checked.
    void SetBrightnessSensorChecker(EventBindingChecker check_brightness_sensor);

    using CborConfigurationManagerBase::ApplyCborConfiguration;

    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) override;
};

} // namespace eerie_leap::domain::display_domain::configuration
