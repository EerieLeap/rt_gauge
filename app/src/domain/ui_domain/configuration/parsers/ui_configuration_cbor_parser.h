#pragma once

#include "eerie_memory.hpp"

#include "configuration/cbor/cbor_ui_config/cbor_ui_config.h"
#include "domain/ui_domain/configuration/parsers/ui_configuration_validator.h"
#include "domain/ui_domain/models/ui_configuration.h"
#include "utilities/type/config_value.h"

namespace eerie_leap::domain::ui_domain::configuration::parsers {

using eerie_leap::domain::ui_domain::models::UiConfiguration;
using eerie_leap::utilities::type::ConfigValue;

class UiConfigurationCborParser {
private:
    UiConfigurationValidator::ChildValidator validate_children_;

public:
    // Bumped whenever the CBOR layout changes; a stored config that does not
    // match is rejected, as a result defaults are regenerated instead of decoding a
    // field-shifted layout into the current struct.
    static constexpr uint32_t CONFIGURATION_VERSION = 1;

    // Serialize and Deserialize both validate; without child rules only domain checks run.
    explicit UiConfigurationCborParser(UiConfigurationValidator::ChildValidator validate_children = {});

    eerie_memory::PmrUniquePtr<CborUiConfig> Serialize(const UiConfiguration& configuration);
    eerie_memory::PmrUniquePtr<UiConfiguration> Deserialize(std::pmr::memory_resource* mr, const CborUiConfig& config);
};

} // namespace eerie_leap::domain::ui_domain::configuration::parsers
