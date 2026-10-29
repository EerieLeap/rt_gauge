#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_display_config/cbor_display_config.h"
#include "configuration/cbor/cbor_serializer.h"
#include "utilities/cbor/cbor_helpers.hpp"
#include "utilities/memory/memory_resource_manager.h"

#include "domain/display_domain/configuration/parsers/display_configuration_cbor_parser.h"
#include "domain/display_domain/models/display_configuration.h"

using eerie_leap::configuration::cbor::CborSerializer;
using eerie_leap::domain::display_domain::configuration::parsers::DisplayConfigurationCborParser;
using eerie_leap::domain::display_domain::models::DisplayConfiguration;
using eerie_leap::utilities::cbor::CborHelpers;
using eerie_leap::utilities::memory::Mrm;

namespace {

template<typename Fn>
std::string ThrownMessage(Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch(const std::exception& e) {
        return e.what();
    } catch(...) {
        return "unexpected exception";
    }

    return "no exception";
}

// The text of @p sensor_id is not copied: it has to outlive the result.
CborDisplayConfig MakeCborConfig(std::string_view sensor_id) {
    CborDisplayConfig config{};
    config.version = DisplayConfigurationCborParser::CONFIGURATION_VERSION;
    config.brightness = 200;
    config.blanking_enabled = true;
    config.screen_timeout_s = 45;
    config.theme_id = 3;
    config.brightness_sensor_id = CborHelpers::ToZcborString(sensor_id);

    return config;
}

DisplayConfiguration MakeConfiguration(std::string_view sensor_id) {
    DisplayConfiguration configuration;
    configuration.brightness_sensor_id = sensor_id;

    return configuration;
}

std::string_view View(const DisplayConfiguration& configuration) {
    return configuration.brightness_sensor_id;
}

} // namespace

ZTEST_SUITE(display_configuration_parser, NULL, NULL, NULL, NULL, NULL);

ZTEST(display_configuration_parser, test_the_brightness_sensor_id_round_trips) {
    const auto configuration = MakeConfiguration("ambient_light");

    auto config = DisplayConfigurationCborParser::Serialize(configuration);
    zassert_equal(config->version, DisplayConfigurationCborParser::CONFIGURATION_VERSION);
    zassert_equal(CborHelpers::ToStringView(config->brightness_sensor_id), "ambient_light");

    auto restored = DisplayConfigurationCborParser::Deserialize(Mrm::GetDefaultPmr(), *config);

    zassert_equal(View(*restored), "ambient_light");
}

// None is the default, and what a configuration without the field decodes to.
ZTEST(display_configuration_parser, test_no_brightness_sensor_is_accepted) {
    const DisplayConfiguration configuration{};
    zassert_true(configuration.brightness_sensor_id.empty());

    auto config = DisplayConfigurationCborParser::Serialize(configuration);
    zassert_equal(config->brightness_sensor_id.len, 0U);
    zassert_true(
        DisplayConfigurationCborParser::Deserialize(Mrm::GetDefaultPmr(), *config)->brightness_sensor_id.empty()
    );

    auto unset = MakeCborConfig("");
    unset.brightness_sensor_id = {};
    zassert_true(
        DisplayConfigurationCborParser::Deserialize(Mrm::GetDefaultPmr(), unset)->brightness_sensor_id.empty()
    );
}

ZTEST(display_configuration_parser, test_the_text_the_codec_carries_is_the_text_that_was_stored) {
    // Longer than 23 bytes, so its length takes a byte of its own.
    const auto configuration = MakeConfiguration("a_sensor_with_a_rather_long_identifier_2");

    CborSerializer<CborDisplayConfig> serializer;
    const auto bytes = serializer.Serialize(*DisplayConfigurationCborParser::Serialize(configuration));
    zassert_false(bytes.empty(), "Expected the configuration to encode.");

    const auto decoded = serializer.Deserialize(bytes);
    zassert_not_null(decoded.get());

    auto restored = DisplayConfigurationCborParser::Deserialize(Mrm::GetDefaultPmr(), *decoded);

    zassert_equal(View(*restored), "a_sensor_with_a_rather_long_identifier_2");
    zassert_equal(restored->brightness, configuration.brightness);
}

ZTEST(display_configuration_parser, test_an_id_that_is_a_sensor_id_is_accepted) {
    for(const std::string_view sensor_id : { "a", "A", "_", "ambient_light", "_ambient", "sensor_1", "S1_x_9" }) {
        const auto config = MakeCborConfig(sensor_id);
        auto restored = DisplayConfigurationCborParser::Deserialize(Mrm::GetDefaultPmr(), config);

        zassert_equal(View(*restored), sensor_id);
    }
}

ZTEST(display_configuration_parser, test_an_id_that_does_not_start_like_a_sensor_id_is_refused) {
    for(const std::string_view sensor_id : { "1abc", "9", "-a", " a", ".a" }) {
        const auto config = MakeCborConfig(sensor_id);
        const auto message = ThrownMessage([&] {
            DisplayConfigurationCborParser::Deserialize(Mrm::GetDefaultPmr(), config);
        });

        zassert_equal(
            message,
            "Invalid Display configuration. Brightness sensor ID must start with a letter or an underscore. Value: "
                + std::string(sensor_id) + ".",
            "%s", message.c_str()
        );
    }
}

ZTEST(display_configuration_parser, test_an_id_with_a_character_a_sensor_id_cannot_have_is_refused) {
    for(const std::string_view sensor_id : { "a-b", "a b", "a.b", "a/b", "ab " }) {
        const auto config = MakeCborConfig(sensor_id);
        const auto message = ThrownMessage([&] {
            DisplayConfigurationCborParser::Deserialize(Mrm::GetDefaultPmr(), config);
        });

        zassert_equal(
            message,
            "Invalid Display configuration. Brightness sensor ID must contain only letters, digits, and underscores. "
            "Value: "
                + std::string(sensor_id) + ".",
            "%s", message.c_str()
        );
    }
}

// Deserialize has to accept what Serialize produces, so Serialize refuses what Deserialize would.
ZTEST(display_configuration_parser, test_serialize_refuses_an_id_that_is_not_a_sensor_id) {
    for(const std::string_view sensor_id : { "1abc", "a-b" }) {
        const auto configuration = MakeConfiguration(sensor_id);
        const auto message = ThrownMessage([&] {
            DisplayConfigurationCborParser::Serialize(configuration);
        });

        zassert_true(
            message.starts_with("Invalid Display configuration. Brightness sensor ID must "), "%s", message.c_str()
        );
    }
}
