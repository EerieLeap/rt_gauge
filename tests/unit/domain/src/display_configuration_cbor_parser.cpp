#include <cstdint>
#include <stdexcept>
#include <string>

#include <zephyr/ztest.h>

#include "utilities/memory/memory_resource_manager.h"

#include "configuration/cbor/cbor_display_config/cbor_display_config.h"
#include "domain/display_domain/configuration/parsers/display_configuration_cbor_parser.h"
#include "domain/display_domain/models/display_configuration.h"

using eerie_leap::domain::display_domain::configuration::parsers::DisplayConfigurationCborParser;
using eerie_leap::domain::display_domain::models::DisplayConfiguration;
using eerie_leap::utilities::memory::Mrm;

namespace {

template<typename Fn>
std::string ThrownMessage(Fn&& fn) {
    try {
        fn();
    } catch(const std::exception& e) {
        return e.what();
    } catch(...) {
        return "unexpected exception";
    }

    return "no exception";
}

CborDisplayConfig MakeCborConfig() {
    return CborDisplayConfig{ .version = DisplayConfigurationCborParser::configuration_version,
        .brightness = 200,
        .blanking_enabled = true,
        .screen_timeout_s = 45,
        .theme_id = 3 };
}

} // namespace

ZTEST_SUITE(display_configuration_cbor_parser, NULL, NULL, NULL, NULL, NULL);

ZTEST(display_configuration_cbor_parser, test_defaults_leave_the_panel_lit) {
    DisplayConfiguration configuration;

    zassert_equal(configuration.brightness, 160);
    zassert_false(configuration.blanking_enabled);
    zassert_equal(configuration.screen_timeout_s, 0);
    zassert_equal(configuration.theme_id, 0);
}

ZTEST(display_configuration_cbor_parser, test_serialize_stamps_the_current_version) {
    DisplayConfigurationCborParser parser;

    DisplayConfiguration configuration;
    configuration.brightness = 12;

    auto config = parser.Serialize(configuration);

    zassert_equal(config->version, DisplayConfigurationCborParser::configuration_version);
    zassert_equal(config->brightness, 12);
}

ZTEST(display_configuration_cbor_parser, test_round_trip_preserves_every_field) {
    DisplayConfigurationCborParser parser;

    DisplayConfiguration configuration;
    configuration.brightness = 77;
    configuration.blanking_enabled = true;
    configuration.screen_timeout_s = 30;
    configuration.theme_id = 2;

    auto config = parser.Serialize(configuration);
    auto restored = parser.Deserialize(Mrm::GetDefaultPmr(), *config);

    zassert_equal(restored->brightness, 77);
    zassert_true(restored->blanking_enabled);
    zassert_equal(restored->screen_timeout_s, 30);
    zassert_equal(restored->theme_id, 2);
}

ZTEST(display_configuration_cbor_parser, test_deserialize_accepts_the_current_version) {
    DisplayConfigurationCborParser parser;

    auto config = MakeCborConfig();
    auto restored = parser.Deserialize(Mrm::GetDefaultPmr(), config);

    zassert_equal(restored->brightness, 200);
    zassert_true(restored->blanking_enabled);
    zassert_equal(restored->screen_timeout_s, 45);
    zassert_equal(restored->theme_id, 3);
}

// A stored config written by another layout must fail loudly so the manager
// regenerates defaults instead of reading field-shifted values.
ZTEST(display_configuration_cbor_parser, test_deserialize_rejects_another_version) {
    DisplayConfigurationCborParser parser;

    auto config = MakeCborConfig();
    config.version = DisplayConfigurationCborParser::configuration_version + 1;

    auto message = ThrownMessage([&] {
        parser.Deserialize(Mrm::GetDefaultPmr(), config);
    });

    zassert_equal(
        message,
        "Invalid Display configuration. Unsupported version: "
            + std::to_string(DisplayConfigurationCborParser::configuration_version + 1)
            + ". Expected: " + std::to_string(DisplayConfigurationCborParser::configuration_version) + ".",
        "%s", message.c_str()
    );
}

// The schema stores brightness as a uint; the model is a uint8_t.
ZTEST(display_configuration_cbor_parser, test_deserialize_rejects_out_of_range_brightness) {
    DisplayConfigurationCborParser parser;

    auto config = MakeCborConfig();
    config.brightness = 256;

    auto message = ThrownMessage([&] {
        parser.Deserialize(Mrm::GetDefaultPmr(), config);
    });

    zassert_equal(
        message, "Invalid Display configuration. Brightness must be between 0 and 255. Value: 256.", "%s",
        message.c_str()
    );
}

ZTEST(display_configuration_cbor_parser, test_deserialize_accepts_the_brightness_bounds) {
    DisplayConfigurationCborParser parser;

    auto config = MakeCborConfig();
    config.brightness = 255;
    zassert_equal(parser.Deserialize(Mrm::GetDefaultPmr(), config)->brightness, 255);

    config.brightness = 0;
    zassert_equal(parser.Deserialize(Mrm::GetDefaultPmr(), config)->brightness, 0);
}
