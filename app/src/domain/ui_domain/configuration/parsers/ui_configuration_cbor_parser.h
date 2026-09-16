#pragma once

#include "eerie_memory.hpp"

#include "utilities/type/config_value.h"
#include "configuration/cbor/cbor_ui_config/cbor_ui_config.h"
#include "domain/ui_domain/models/ui_configuration.h"

namespace eerie_leap::domain::ui_domain::configuration::parsers {

using eerie_leap::utilities::type::ConfigValue;
using eerie_leap::domain::ui_domain::models::UiConfiguration;

class UiConfigurationCborParser {
public:
    // Bumped whenever the CBOR layout changes; a stored config that does not
    // match is rejected, as a result defaults are regenerated instead of decoding a
    // field-shifted layout into the current struct.
    static constexpr uint32_t configuration_version = 2;

    UiConfigurationCborParser() = default;

    eerie_memory::pmr_unique_ptr<CborUiConfig> Serialize(const UiConfiguration& configuration);
    eerie_memory::pmr_unique_ptr<UiConfiguration> Deserialize(std::pmr::memory_resource* mr, const CborUiConfig& config);
};

} // namespace eerie_leap::domain::ui_domain::configuration::parsers
