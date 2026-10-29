#include <cctype>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

#include "utilities/cbor/cbor_helpers.hpp"
#include "utilities/memory/memory_resource_manager.h"

#include "display_configuration_cbor_parser.h"

namespace eerie_leap::domain::display_domain::configuration::parsers {

using namespace eerie_memory;
using namespace eerie_leap::utilities::cbor;
using namespace eerie_leap::utilities::memory;

static void InvalidDisplayConfiguration(std::string_view message) {
    throw std::invalid_argument("Invalid Display configuration. " + std::string(message));
}

// Empty names no sensor. Otherwise the rule SensorValidator keeps a sensor's ID to: it starts with a
// letter or an underscore and holds letters, digits and underscores only.
static void ValidateBrightnessSensorId(std::string_view sensor_id) {
    if(sensor_id.empty())
        return;

    const auto is_letter = [](char c) {
        return std::isalpha(static_cast<unsigned char>(c)) != 0;
    };

    if(!is_letter(sensor_id.front()) && sensor_id.front() != '_')
        InvalidDisplayConfiguration(
            "Brightness sensor ID must start with a letter or an underscore. Value: " + std::string(sensor_id) + "."
        );

    for(const char c : sensor_id) {
        if(!is_letter(c) && std::isdigit(static_cast<unsigned char>(c)) == 0 && c != '_')
            InvalidDisplayConfiguration(
                "Brightness sensor ID must contain only letters, digits, and underscores. Value: "
                + std::string(sensor_id) + "."
            );
    }
}

// The CBOR form points into the configuration's string, so it is valid while the configuration lives.
PmrUniquePtr<CborDisplayConfig> DisplayConfigurationCborParser::Serialize(const DisplayConfiguration& configuration) {
    ValidateBrightnessSensorId(configuration.brightness_sensor_id);

    auto config = make_unique_pmr<CborDisplayConfig>(Mrm::GetExtPmr());

    config->version = CONFIGURATION_VERSION;
    config->brightness = configuration.brightness;
    config->blanking_enabled = configuration.blanking_enabled;
    config->screen_timeout_s = configuration.screen_timeout_s;
    config->theme_id = configuration.theme_id;
    config->brightness_sensor_id = CborHelpers::ToZcborString(configuration.brightness_sensor_id);

    return config;
}

PmrUniquePtr<DisplayConfiguration>
DisplayConfigurationCborParser::Deserialize(std::pmr::memory_resource* mr, const CborDisplayConfig& config) {

    if(config.version != CONFIGURATION_VERSION)
        InvalidDisplayConfiguration(
            "Unsupported version: " + std::to_string(config.version)
            + ". Expected: " + std::to_string(CONFIGURATION_VERSION) + "."
        );

    if(config.brightness > std::numeric_limits<uint8_t>::max())
        InvalidDisplayConfiguration(
            "Brightness must be between 0 and 255. Value: " + std::to_string(config.brightness) + "."
        );

    const auto brightness_sensor_id = CborHelpers::ToStringView(config.brightness_sensor_id);
    ValidateBrightnessSensorId(brightness_sensor_id);

    auto configuration = make_unique_pmr<DisplayConfiguration>(mr);

    configuration->brightness = static_cast<uint8_t>(config.brightness);
    configuration->blanking_enabled = config.blanking_enabled;
    configuration->screen_timeout_s = config.screen_timeout_s;
    configuration->theme_id = config.theme_id;
    configuration->brightness_sensor_id.assign(brightness_sensor_id);

    return configuration;
}

} // namespace eerie_leap::domain::display_domain::configuration::parsers
