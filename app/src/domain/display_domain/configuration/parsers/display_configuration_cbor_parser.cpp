#include <limits>
#include <stdexcept>
#include <string>

#include "utilities/memory/memory_resource_manager.h"

#include "display_configuration_cbor_parser.h"

namespace eerie_leap::domain::display_domain::configuration::parsers {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;

static void InvalidDisplayConfiguration(std::string_view message) {
    throw std::invalid_argument("Invalid Display configuration. " + std::string(message));
}

pmr_unique_ptr<CborDisplayConfig> DisplayConfigurationCborParser::Serialize(const DisplayConfiguration& configuration) {
    auto config = make_unique_pmr<CborDisplayConfig>(Mrm::GetExtPmr());

    config->version = configuration_version;
    config->brightness = configuration.brightness;
    config->blanking_enabled = configuration.blanking_enabled;
    config->screen_timeout_s = configuration.screen_timeout_s;
    config->theme_id = configuration.theme_id;

    return config;
}

pmr_unique_ptr<DisplayConfiguration>
DisplayConfigurationCborParser::Deserialize(std::pmr::memory_resource* mr, const CborDisplayConfig& config) {

    if(config.version != configuration_version)
        InvalidDisplayConfiguration(
            "Unsupported version: " + std::to_string(config.version)
            + ". Expected: " + std::to_string(configuration_version) + "."
        );

    if(config.brightness > std::numeric_limits<uint8_t>::max())
        InvalidDisplayConfiguration(
            "Brightness must be between 0 and 255. Value: " + std::to_string(config.brightness) + "."
        );

    auto configuration = make_unique_pmr<DisplayConfiguration>(mr);

    configuration->brightness = static_cast<uint8_t>(config.brightness);
    configuration->blanking_enabled = config.blanking_enabled;
    configuration->screen_timeout_s = config.screen_timeout_s;
    configuration->theme_id = config.theme_id;

    return configuration;
}

} // namespace eerie_leap::domain::display_domain::configuration::parsers
