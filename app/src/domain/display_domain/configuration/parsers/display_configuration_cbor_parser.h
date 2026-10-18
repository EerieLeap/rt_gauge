#pragma once

#include <cstdint>

#include "eerie_memory.hpp"

#include "configuration/cbor/cbor_display_config/cbor_display_config.h"
#include "domain/display_domain/models/display_configuration.h"

namespace eerie_leap::domain::display_domain::configuration::parsers {

using eerie_leap::domain::display_domain::models::DisplayConfiguration;

class DisplayConfigurationCborParser {
public:
    // Bumped whenever the CBOR layout changes; a stored config that does not
    // match is rejected so defaults are regenerated instead of decoding a
    // field-shifted layout into the current struct.
    static constexpr uint32_t CONFIGURATION_VERSION = 1;

    DisplayConfigurationCborParser() = default;

    static eerie_memory::PmrUniquePtr<CborDisplayConfig> Serialize(const DisplayConfiguration& configuration);
    static eerie_memory::PmrUniquePtr<DisplayConfiguration>
    Deserialize(std::pmr::memory_resource* mr, const CborDisplayConfig& config);
};

} // namespace eerie_leap::domain::display_domain::configuration::parsers
