#include <array>
#include <memory>
#include <vector>
#include <stdexcept>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/cbor/cbor_helpers.hpp"
#include "configuration/services/cbor_configuration_service.h"
#include "configuration/cbor/cbor_ui_config/cbor_ui_config_cbor_encode.h"
#include "configuration/cbor/cbor_ui_config/cbor_ui_config_cbor_decode.h"
#include "configuration/cbor/cbor_ui_config/cbor_ui_config_size.h"

#include "domain/ui_domain/models/ui_configuration.h"
#include "domain/ui_domain/models/widget_type.h"
#include "domain/ui_domain/models/widget_property.h"
#include "domain/ui_domain/models/icon_type.h"
#include "domain/ui_domain/models/widget_direction.h"
#include "domain/ui_domain/models/widget_fill_mode.h"
#include "domain/ui_domain/configuration/parsers/ui_configuration_cbor_parser.h"

#include "views/widgets/indicators/horizontal_chart_indicator/horizontal_chart_indicator.h"

using namespace eerie_memory;
using namespace eerie_leap::configuration::services;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::domain::ui_domain::configuration;
using namespace eerie_leap::domain::ui_domain::configuration::parsers;
using namespace eerie_leap::views::widgets::indicators;

ZTEST_SUITE(ui_configuration_parser, NULL, NULL, NULL, NULL, NULL);

pmr_unique_ptr<UiConfiguration> ui_configuration_parser_GetTestUiConfiguration() {
    auto ui_configuration = make_unique_pmr<UiConfiguration>(Mrm::GetDefaultPmr());
    ui_configuration->active_screen_group_id = 3;

    auto screen_configuration = make_shared_pmr<ScreenConfiguration>(Mrm::GetDefaultPmr());
    screen_configuration->id = 8;
    screen_configuration->screen_group_id = 3;
    screen_configuration->type = ScreenType::Gauge;
    screen_configuration->z_index = -2;
    screen_configuration->is_visible = true;
    screen_configuration->is_overlay = true;

    screen_configuration->grid.snap_enabled = true;
    screen_configuration->grid.width = 3;
    screen_configuration->grid.height = 3;
    screen_configuration->grid.spacing_px = 0;

    // First widget
    auto widget1 = make_shared_pmr<WidgetConfiguration>(Mrm::GetDefaultPmr());
    widget1->type = WidgetType::IndicatorArcFill;
    widget1->id = 0;
    widget1->position_grid.x = 0;
    widget1->position_grid.y = 0;
    widget1->size_grid.width = 3;
    widget1->size_grid.height = 3;
    widget1->z_index = -1;
    widget1->properties[WidgetPropertyType::MIN_VALUE] = 0;
    widget1->properties[WidgetPropertyType::MAX_VALUE] = 100;
    widget1->properties[WidgetPropertyType::LABEL] = "sensor_1";

    // Two targets fed by one source, which is the fan-out the binding list exists for.
    PropertyBinding value_binding;
    value_binding.target = WidgetPropertyType::VALUE;
    value_binding.channel = EventChannelId::Sensors;
    value_binding.event_type = 0;
    value_binding.payload_key = 1;
    value_binding.selector_key = 0;
    value_binding.selector_value = std::pmr::string("sensor_1");
    widget1->bindings.push_back(std::move(value_binding));

    PropertyBinding visibility_binding;
    visibility_binding.target = WidgetPropertyType::IS_VISIBLE;
    visibility_binding.channel = EventChannelId::Sensors;
    visibility_binding.event_type = 0;
    visibility_binding.payload_key = 1;
    visibility_binding.selector_key = 0;
    visibility_binding.selector_value = std::pmr::string("sensor_1");
    widget1->bindings.push_back(std::move(visibility_binding));

    screen_configuration->AddWidget(std::move(widget1));

    // Second widget
    auto widget2 = make_shared_pmr<WidgetConfiguration>(Mrm::GetDefaultPmr());
    widget2->type = WidgetType::IndicatorDigital;
    widget2->id = 1;
    widget2->position_grid.x = 1;
    widget2->position_grid.y = 1;
    widget2->size_grid.width = 1;
    widget2->size_grid.height = 1;
    widget2->properties[WidgetPropertyType::IS_VISIBLE] = false;
    widget2->properties[WidgetPropertyType::MIN_VALUE] = 0;
    widget2->properties[WidgetPropertyType::MAX_VALUE] = 100;
    widget2->properties[WidgetPropertyType::LABEL] = "sensor_1";

    // Two-way and unconditional: the selector stays unset and the outbound event carries the write.
    PropertyBinding setting_binding;
    setting_binding.target = WidgetPropertyType::VALUE;
    setting_binding.channel = EventChannelId::Settings;
    setting_binding.event_type = 0;
    setting_binding.payload_key = 1;
    setting_binding.direction = PropertyBindingDirection::InOut;
    setting_binding.outbound_event_type = 2;
    widget2->bindings.push_back(std::move(setting_binding));

    screen_configuration->AddWidget(std::move(widget2));

    // Third widget
    auto widget3 = make_shared_pmr<WidgetConfiguration>(Mrm::GetDefaultPmr());
    widget3->type = WidgetType::IndicatorHorizontalChart;
    widget3->id = 2;
    widget3->position_grid.x = 0;
    widget3->position_grid.y = 0;
    widget3->size_grid.width = 3;
    widget3->size_grid.height = 1;
    widget3->z_index = 2;
    widget3->properties[WidgetPropertyType::MIN_VALUE] = 0;
    widget3->properties[WidgetPropertyType::MAX_VALUE] = 100;
    widget3->properties[WidgetPropertyType::LABEL] = "sensor_1";
    widget3->properties[WidgetPropertyType::CHART_POINT_COUNT] = 35;
    widget3->properties[WidgetPropertyType::CHART_TYPE] = static_cast<std::uint16_t>(HorizontalChartIndicatorType::Line);
    screen_configuration->AddWidget(std::move(widget3));

    ui_configuration->screen_configurations.push_back(std::move(screen_configuration));

    return ui_configuration;
}

void ui_configuration_parser_CompareUiConfigurations(UiConfiguration& ui_configuration, UiConfiguration& deserialized_ui_configuration) {
    zassert_equal(deserialized_ui_configuration.active_screen_group_id, ui_configuration.active_screen_group_id);

    for(std::size_t i = 0; i < ui_configuration.screen_configurations.size(); i++) {
        zassert_equal(deserialized_ui_configuration.screen_configurations[i]->id, ui_configuration.screen_configurations[i]->id);
        zassert_equal(deserialized_ui_configuration.screen_configurations[i]->screen_group_id, ui_configuration.screen_configurations[i]->screen_group_id);
        zassert_equal(deserialized_ui_configuration.screen_configurations[i]->type, ui_configuration.screen_configurations[i]->type);
        zassert_equal(deserialized_ui_configuration.screen_configurations[i]->z_index, ui_configuration.screen_configurations[i]->z_index);
        zassert_equal(deserialized_ui_configuration.screen_configurations[i]->is_visible, ui_configuration.screen_configurations[i]->is_visible);
        zassert_equal(deserialized_ui_configuration.screen_configurations[i]->is_overlay, ui_configuration.screen_configurations[i]->is_overlay);
        zassert_equal(deserialized_ui_configuration.screen_configurations[i]->grid.snap_enabled, ui_configuration.screen_configurations[i]->grid.snap_enabled);
        zassert_equal(deserialized_ui_configuration.screen_configurations[i]->grid.width, ui_configuration.screen_configurations[i]->grid.width);
        zassert_equal(deserialized_ui_configuration.screen_configurations[i]->grid.height, ui_configuration.screen_configurations[i]->grid.height);
        zassert_equal(deserialized_ui_configuration.screen_configurations[i]->grid.spacing_px, ui_configuration.screen_configurations[i]->grid.spacing_px);

        for(std::size_t j = 0; j < ui_configuration.screen_configurations[i]->widget_configurations.size(); j++) {
            zassert_equal(deserialized_ui_configuration.screen_configurations[i]->widget_configurations[j]->type, ui_configuration.screen_configurations[i]->widget_configurations[j]->type);
            zassert_equal(deserialized_ui_configuration.screen_configurations[i]->widget_configurations[j]->id, ui_configuration.screen_configurations[i]->widget_configurations[j]->id);
            zassert_equal(deserialized_ui_configuration.screen_configurations[i]->widget_configurations[j]->position_grid.x, ui_configuration.screen_configurations[i]->widget_configurations[j]->position_grid.x);
            zassert_equal(deserialized_ui_configuration.screen_configurations[i]->widget_configurations[j]->position_grid.y, ui_configuration.screen_configurations[i]->widget_configurations[j]->position_grid.y);
            zassert_equal(deserialized_ui_configuration.screen_configurations[i]->widget_configurations[j]->size_grid.width, ui_configuration.screen_configurations[i]->widget_configurations[j]->size_grid.width);
            zassert_equal(deserialized_ui_configuration.screen_configurations[i]->widget_configurations[j]->size_grid.height, ui_configuration.screen_configurations[i]->widget_configurations[j]->size_grid.height);
            zassert_equal(deserialized_ui_configuration.screen_configurations[i]->widget_configurations[j]->z_index, ui_configuration.screen_configurations[i]->widget_configurations[j]->z_index);
            zassert_equal(deserialized_ui_configuration.screen_configurations[i]->widget_configurations[j]->properties.size(), ui_configuration.screen_configurations[i]->widget_configurations[j]->properties.size());
            for(auto& property : ui_configuration.screen_configurations[i]->widget_configurations[j]->properties) {
                zassert_true(deserialized_ui_configuration.screen_configurations[i]->widget_configurations[j]->properties[property.first] == ui_configuration.screen_configurations[i]->widget_configurations[j]->properties[property.first]);
            }

            const auto& bindings = ui_configuration.screen_configurations[i]->widget_configurations[j]->bindings;
            const auto& deserialized_bindings = deserialized_ui_configuration.screen_configurations[i]->widget_configurations[j]->bindings;

            zassert_equal(deserialized_bindings.size(), bindings.size());

            for(std::size_t k = 0; k < bindings.size(); k++) {
                zassert_equal(deserialized_bindings[k].target, bindings[k].target);
                zassert_equal(deserialized_bindings[k].channel, bindings[k].channel);
                zassert_equal(deserialized_bindings[k].event_type, bindings[k].event_type);
                zassert_equal(deserialized_bindings[k].payload_key, bindings[k].payload_key);
                zassert_equal(deserialized_bindings[k].direction, bindings[k].direction);
                zassert_equal(deserialized_bindings[k].outbound_event_type, bindings[k].outbound_event_type);
                zassert_equal(deserialized_bindings[k].selector_key, bindings[k].selector_key);
                zassert_equal(deserialized_bindings[k].HasSelector(), bindings[k].HasSelector());
                zassert_true(deserialized_bindings[k].selector_value == bindings[k].selector_value);
            }
        }
    }
}

ZTEST(ui_configuration_parser, test_CborSerializeDeserialize) {
    UiConfigurationCborParser ui_configuration_cbor_parser;

    auto ui_configuration = ui_configuration_parser_GetTestUiConfiguration();

    auto serialized_ui_configuration = ui_configuration_cbor_parser.Serialize(*ui_configuration);
    auto deserialized_ui_configuration = ui_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_ui_configuration.get());

    ui_configuration_parser_CompareUiConfigurations(
        *ui_configuration, *deserialized_ui_configuration);
}

ZTEST(ui_configuration_parser, test_shape_properties_and_bindings_round_trip) {
    auto configuration = ui_configuration_parser_GetTestUiConfiguration();
    auto& widget = *configuration->screen_configurations[0]->widget_configurations[0];
    widget.type = WidgetType::BasicIcon;
    widget.properties.clear();
    widget.properties[WidgetPropertyType::ICON_TYPE] = static_cast<int>(IconType::TriangleRight);
    widget.properties[WidgetPropertyType::WIDTH_PX] = 80;
    widget.properties[WidgetPropertyType::HEIGHT_PX] = 40;
    widget.properties[WidgetPropertyType::STROKE_PX] = 3;
    widget.properties[WidgetPropertyType::CORNER_RAD_PX] = 8;
    widget.properties[WidgetPropertyType::FILL_MODE] = static_cast<int>(WidgetFillMode::Outline);
    widget.properties[WidgetPropertyType::DIRECTION] = static_cast<int>(WidgetDirection::TopToBottom);
    widget.bindings[0].target = WidgetPropertyType::WIDTH_PX;
    widget.bindings[1].target = WidgetPropertyType::FILL_MODE;

    UiConfigurationCborParser parser;
    auto serialized = parser.Serialize(*configuration);
    auto deserialized = parser.Deserialize(Mrm::GetDefaultPmr(), *serialized);
    ui_configuration_parser_CompareUiConfigurations(*configuration, *deserialized);
}

ZTEST(ui_configuration_parser, test_CborRoundTripKeepsBindingDetail) {
    UiConfigurationCborParser parser;

    auto ui_configuration = ui_configuration_parser_GetTestUiConfiguration();
    auto serialized = parser.Serialize(*ui_configuration);
    auto deserialized = parser.Deserialize(Mrm::GetDefaultPmr(), *serialized.get());

    const auto& widgets = deserialized->screen_configurations[0]->widget_configurations;

    const auto& fanned_out = widgets[0]->bindings;
    zassert_equal(fanned_out.size(), 2U);

    // Same source, two targets.
    zassert_equal(fanned_out[0].channel, EventChannelId::Sensors);
    zassert_equal(fanned_out[1].channel, EventChannelId::Sensors);
    zassert_equal(fanned_out[0].payload_key, fanned_out[1].payload_key);
    zassert_not_equal(fanned_out[0].target, fanned_out[1].target);

    zassert_true(fanned_out[0].HasSelector());
    zassert_true(std::get<std::pmr::string>(fanned_out[0].selector_value) == "sensor_1");

    const auto& two_way = widgets[1]->bindings;
    zassert_equal(two_way.size(), 1U);
    zassert_equal(two_way[0].direction, PropertyBindingDirection::InOut);
    zassert_equal(two_way[0].outbound_event_type, 2U);

    // An unset selector must not come back as a decoded value.
    zassert_false(two_way[0].HasSelector());
}

ZTEST(ui_configuration_parser, test_CborRoundTripKeepsAWidgetWithoutBindings) {
    UiConfigurationCborParser parser;

    auto ui_configuration = ui_configuration_parser_GetTestUiConfiguration();
    auto serialized = parser.Serialize(*ui_configuration);
    auto deserialized = parser.Deserialize(Mrm::GetDefaultPmr(), *serialized.get());

    zassert_true(deserialized->screen_configurations[0]->widget_configurations[2]->bindings.empty());
}

// is_visible and is_overlay are adjacent bools on the wire, so only distinct
// values catch a field shift between them.
ZTEST(ui_configuration_parser, test_CborRoundTripKeepsOverlayApartFromVisibility) {
    UiConfigurationCborParser parser;

    auto ui_configuration = ui_configuration_parser_GetTestUiConfiguration();
    auto& screen = *ui_configuration->screen_configurations[0];
    screen.is_visible = false;
    screen.is_overlay = true;

    auto serialized = parser.Serialize(*ui_configuration);

    zassert_false(serialized->CborScreenConfig_m[0].is_visible);
    zassert_true(serialized->CborScreenConfig_m[0].is_overlay);

    auto deserialized = parser.Deserialize(Mrm::GetDefaultPmr(), *serialized.get());

    zassert_false(deserialized->screen_configurations[0]->is_visible);
    zassert_true(deserialized->screen_configurations[0]->is_overlay);
}

ZTEST(ui_configuration_parser, test_AScreenIsNotAnOverlayByDefault) {
    auto screen_configuration = make_shared_pmr<ScreenConfiguration>(Mrm::GetDefaultPmr());

    zassert_false(screen_configuration->is_overlay);
}

ZTEST(ui_configuration_parser, test_CborSerializeStampsTheCurrentVersion) {
    UiConfigurationCborParser parser;

    auto ui_configuration = ui_configuration_parser_GetTestUiConfiguration();
    auto serialized = parser.Serialize(*ui_configuration);

    zassert_equal(serialized->version, UiConfigurationCborParser::configuration_version);
}

ZTEST(ui_configuration_parser, test_CborDeserializeRejectsAnotherVersion) {
    UiConfigurationCborParser parser;

    auto ui_configuration = ui_configuration_parser_GetTestUiConfiguration();
    auto serialized = parser.Serialize(*ui_configuration);

    serialized->version = UiConfigurationCborParser::configuration_version - 1;

    bool threw = false;

    try {
        parser.Deserialize(Mrm::GetDefaultPmr(), *serialized.get());
    } catch(const std::invalid_argument&) {
        threw = true;
    }

    zassert_true(threw);
}

ZTEST(ui_configuration_parser, test_CborDeserializeOrdersWidgetsByZIndex) {
    UiConfigurationCborParser ui_configuration_cbor_parser;

    auto ui_configuration = ui_configuration_parser_GetTestUiConfiguration();
    auto& widget_configurations = ui_configuration->screen_configurations[0]->widget_configurations;
    widget_configurations[0]->z_index = 5;
    widget_configurations[1]->z_index = 1;
    widget_configurations[2]->z_index = 1;

    auto serialized_ui_configuration = ui_configuration_cbor_parser.Serialize(*ui_configuration);
    auto deserialized_ui_configuration = ui_configuration_cbor_parser.Deserialize(Mrm::GetDefaultPmr(), *serialized_ui_configuration.get());

    auto& deserialized_widget_configurations = deserialized_ui_configuration->screen_configurations[0]->widget_configurations;
    zassert_equal(deserialized_widget_configurations.size(), widget_configurations.size());

    // Equal z-index keeps configuration order.
    zassert_equal(deserialized_widget_configurations[0]->id, widget_configurations[1]->id);
    zassert_equal(deserialized_widget_configurations[1]->id, widget_configurations[2]->id);
    zassert_equal(deserialized_widget_configurations[2]->id, widget_configurations[0]->id);
}

namespace {

bool Deserializes(const CborUiConfig& config) {
    try {
        UiConfigurationCborParser parser;
        parser.Deserialize(Mrm::GetDefaultPmr(), config);
    } catch(const std::invalid_argument&) {
        return false;
    }

    return true;
}

// A version 1 config with one Gauge screen and one BasicIcon widget. Hand-encoded
// to check the wire format independently of our encoder and parser.
std::vector<uint8_t> WidgetPropertyPayload(std::initializer_list<uint8_t> properties) {
    std::vector<uint8_t> payload = {
        0x83, 0x01, 0x03, 0x81,
        0x88, 0x08, 0x03, 0x02, 0x00, 0xf5, 0xf4, 0x84, 0xf5, 0x03, 0x03, 0x00, 0x81,
        0x87, 0x1a, 0x00, 0x01, 0x00, 0x01, 0x00,
        0x82, 0x00, 0x00, 0x82, 0x01, 0x01, 0x00
    };
    payload.insert(payload.end(), properties.begin(), properties.end());
    payload.push_back(0x80); // No bindings.
    return payload;
}

} // namespace

ZTEST(ui_configuration_parser, test_widget_properties_round_trip_with_empty_ui_properties) {
    auto configuration = ui_configuration_parser_GetTestUiConfiguration();
    auto& widget = *configuration->screen_configurations[0]->widget_configurations[0];
    auto& properties = widget.properties;
    properties[WidgetPropertyType::MAX_VALUE] = 100.5;
    properties[WidgetPropertyType::IS_SMOOTHED] = true;
    properties[WidgetPropertyType::WIDTH_PX] = 80;
    properties[WidgetPropertyType::FILL_MODE] = static_cast<int>(WidgetFillMode::Outline);
    const std::pair<WidgetPropertyType, const char*> colors[] = {
        { WidgetPropertyType::COLOR_PRIMARY_ACTIVE, "#3366fF80" },
        { WidgetPropertyType::COLOR_PRIMARY_INACTIVE, "#00000000" },
        { WidgetPropertyType::COLOR_SECONDARY_ACTIVE, "#FFFFFFFF" },
        { WidgetPropertyType::COLOR_SECONDARY_INACTIVE, "" },
        { WidgetPropertyType::COLOR_TERTIARY_ACTIVE, "#12345678" },
        { WidgetPropertyType::COLOR_TERTIARY_INACTIVE, "#abcdef01" }
    };
    for(const auto& [type, text] : colors) {
        properties[type] = std::pmr::string(text, Mrm::GetExtPmr());
        auto binding = widget.bindings.front();
        binding.target = type;
        widget.bindings.push_back(std::move(binding));
    }
    properties[WidgetPropertyType::OPACITY] = 128;
    auto opacity_binding = widget.bindings.front();
    opacity_binding.target = WidgetPropertyType::OPACITY;
    widget.bindings.push_back(std::move(opacity_binding));
    // Also exercise omission of the optional widget property map.
    configuration->screen_configurations[0]->widget_configurations[2]->properties.clear();

    UiConfigurationCborParser parser;
    auto serialized = parser.Serialize(*configuration);
    zassert_equal(serialized->version, 1);
    zassert_false(serialized->properties_present);
    std::vector<uint8_t> payload(cbor_get_size_CborUiConfig(*serialized));
    size_t encoded_size = 0;
    zassert_equal(cbor_encode_CborUiConfig(payload.data(), payload.size(), serialized.get(), &encoded_size), 0);
    zassert_equal(encoded_size, payload.size());

    auto decoded = make_unique_pmr<CborUiConfig>(Mrm::GetDefaultPmr());
    size_t decoded_size = 0;
    zassert_equal(cbor_decode_CborUiConfig(payload.data(), encoded_size, decoded.get(), &decoded_size), 0);
    zassert_equal(decoded_size, encoded_size);

    const auto& cbor_properties = decoded->CborScreenConfig_m[0].CborWidgetConfig_m[0].properties.CborPropertyValueType_m;
    zassert_equal(cbor_properties.size(), properties.size());
    for(const auto& property : cbor_properties)
        zassert_true(properties.contains(static_cast<WidgetPropertyType>(property.CborPropertyValueType_m_key)));

    auto deserialized = parser.Deserialize(Mrm::GetDefaultPmr(), *decoded);
    ui_configuration_parser_CompareUiConfigurations(*configuration, *deserialized);
    zassert_true(deserialized->properties.empty());
    zassert_true(deserialized->screen_configurations[0]->widget_configurations[2]->properties.empty());
}

ZTEST(ui_configuration_parser, test_deserialize_rejects_malformed_persisted_colors) {
    using eerie_leap::utilities::cbor::CborHelpers;

    auto configuration = ui_configuration_parser_GetTestUiConfiguration();
    auto& properties = configuration->screen_configurations[0]->widget_configurations[0]->properties;
    properties.clear();
    properties[WidgetPropertyType::COLOR_PRIMARY_ACTIVE] = std::pmr::string("#3366FF80");
    UiConfigurationCborParser parser;
    auto serialized = parser.Serialize(*configuration);
    auto& property = serialized->CborScreenConfig_m[0].CborWidgetConfig_m[0].properties.CborPropertyValueType_m[0];
    auto& value = property.CborPropertyValueType_m;
    const std::pmr::string invalid_colors[] = {
        "#3366FF", "3366FF80", "#3366FF8G", "#3366FF800", std::pmr::string("#3366FF80\0", 10)
    };
    for(auto type : { WidgetPropertyType::COLOR_PRIMARY_ACTIVE, WidgetPropertyType::COLOR_PRIMARY_INACTIVE,
            WidgetPropertyType::COLOR_SECONDARY_ACTIVE, WidgetPropertyType::COLOR_SECONDARY_INACTIVE,
            WidgetPropertyType::COLOR_TERTIARY_ACTIVE, WidgetPropertyType::COLOR_TERTIARY_INACTIVE }) {
        property.CborPropertyValueType_m_key = static_cast<uint32_t>(type);
        value.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_tstr_c;
        for(const auto& text : invalid_colors) {
            value.value = CborHelpers::ToZcborString(text);
            zassert_false(Deserializes(*serialized));
        }
        value.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_c;
        value.value = int32_t{128};
        zassert_false(Deserializes(*serialized));
        value.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_float_c;
        value.value = 128.0;
        zassert_false(Deserializes(*serialized));
        value.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_bool_c;
        value.value = false;
        zassert_false(Deserializes(*serialized));
    }
}

ZTEST(ui_configuration_parser, test_deserialize_rejects_invalid_persisted_opacity) {
    auto configuration = ui_configuration_parser_GetTestUiConfiguration();
    auto& properties = configuration->screen_configurations[0]->widget_configurations[0]->properties;
    properties.clear();
    properties[WidgetPropertyType::OPACITY] = 128;
    UiConfigurationCborParser parser;
    auto serialized = parser.Serialize(*configuration);
    auto& value = serialized->CborScreenConfig_m[0].CborWidgetConfig_m[0]
        .properties.CborPropertyValueType_m[0].CborPropertyValueType_m;
    for(int32_t opacity : { 0, 128, 255 }) {
        value.value = opacity;
        zassert_true(Deserializes(*serialized));
    }
    const int32_t invalid_opacities[] = { -1, 256, INT32_MIN, INT32_MAX };
    for(int32_t opacity : invalid_opacities) {
        value.value = opacity;
        zassert_false(Deserializes(*serialized));
    }
    value.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_float_c;
    for(double opacity : { 0.0, 128.5, 255.0 }) {
        value.value = opacity;
        zassert_false(Deserializes(*serialized));
    }
    value.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_bool_c;
    value.value = false;
    zassert_false(Deserializes(*serialized));
    value.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_tstr_c;
    const std::pmr::string opacity_text = "128";
    value.value = eerie_leap::utilities::cbor::CborHelpers::ToZcborString(opacity_text);
    zassert_false(Deserializes(*serialized));
}

ZTEST(ui_configuration_parser, test_decodes_integer_widget_property_keys) {
    auto payload = WidgetPropertyPayload({ 0xa1, 0x18, 0x1d, 0x18, 0x50 }); // {29: 80}
    auto decoded = make_unique_pmr<CborUiConfig>(Mrm::GetDefaultPmr());
    size_t decoded_size = 0;
    zassert_equal(cbor_decode_CborUiConfig(payload.data(), payload.size(), decoded.get(), &decoded_size), 0);
    zassert_equal(decoded_size, payload.size());

    UiConfigurationCborParser parser;
    auto configuration = parser.Deserialize(Mrm::GetDefaultPmr(), *decoded);
    const auto& properties = configuration->screen_configurations[0]->widget_configurations[0]->properties;
    zassert_equal(properties.size(), 1U);
    zassert_equal(std::get<int>(properties.at(WidgetPropertyType::WIDTH_PX)), 80);
}

ZTEST(ui_configuration_parser, test_rejects_legacy_text_widget_property_keys) {
    auto payload = WidgetPropertyPayload({ 0xa1, 0x68, 'W', 'I', 'D', 'T', 'H', '_', 'P', 'X', 0x18, 0x50 });
    auto decoded = make_unique_pmr<CborUiConfig>(Mrm::GetDefaultPmr());
    size_t decoded_size = 0;
    zassert_not_equal(cbor_decode_CborUiConfig(payload.data(), payload.size(), decoded.get(), &decoded_size), 0);
}

ZTEST(ui_configuration_parser, test_rejects_unknown_widget_property_ids_before_narrowing) {
    const std::array<uint32_t, 5> invalid_ids = {
        0, static_cast<uint32_t>(WidgetPropertyType::COUNT), 9999,
        0x10000U + static_cast<uint32_t>(WidgetPropertyType::MIN_VALUE), UINT32_MAX
    };
    UiConfigurationCborParser parser;
    auto configuration = ui_configuration_parser_GetTestUiConfiguration();
    auto serialized = parser.Serialize(*configuration);
    auto& widget = serialized->CborScreenConfig_m[0].CborWidgetConfig_m[0];
    auto& property_id = widget.properties.CborPropertyValueType_m[0].CborPropertyValueType_m_key;
    auto original_id = property_id;
    for(auto id : invalid_ids) {
        property_id = id;
        zassert_false(Deserializes(*serialized), "Accepted invalid property ID %u.", id);
    }
    property_id = original_id;

    for(auto id : invalid_ids) {
        widget.CborPropertyBinding_m[0].target = id;
        zassert_false(Deserializes(*serialized), "Accepted invalid binding target %u.", id);
    }
}

ZTEST(ui_configuration_parser, test_rejects_duplicate_widget_property_ids) {
    UiConfigurationCborParser parser;
    auto configuration = ui_configuration_parser_GetTestUiConfiguration();
    auto serialized = parser.Serialize(*configuration);
    auto& properties = serialized->CborScreenConfig_m[0].CborWidgetConfig_m[0].properties.CborPropertyValueType_m;
    properties[1].CborPropertyValueType_m_key = properties[0].CborPropertyValueType_m_key;

    zassert_false(Deserializes(*serialized));
}

ZTEST(ui_configuration_parser, test_decodes_empty_ui_property_map) {
    // Version 1, active group 0, an explicitly present empty property map, no screens.
    const uint8_t payload[] = { 0x84, 0x01, 0x00, 0xa0, 0x80 };
    auto decoded = make_unique_pmr<CborUiConfig>(Mrm::GetDefaultPmr());
    size_t decoded_size = 0;
    zassert_equal(cbor_decode_CborUiConfig(payload, sizeof(payload), decoded.get(), &decoded_size), 0);
    zassert_equal(decoded_size, sizeof(payload));
    zassert_true(decoded->properties_present);

    UiConfigurationCborParser parser;
    auto configuration = parser.Deserialize(Mrm::GetDefaultPmr(), *decoded);
    zassert_true(configuration->properties.empty());
}

ZTEST(ui_configuration_parser, test_rejects_widget_property_ids_in_ui_properties) {
    // WIDTH_PX (29) belongs to WidgetPropertyType, not UiPropertyType.
    const uint8_t payload[] = { 0x84, 0x01, 0x00, 0xa1, 0x18, 0x1d, 0x18, 0x50, 0x80 };
    auto decoded = make_unique_pmr<CborUiConfig>(Mrm::GetDefaultPmr());
    size_t decoded_size = 0;
    zassert_equal(cbor_decode_CborUiConfig(payload, sizeof(payload), decoded.get(), &decoded_size), 0);
    zassert_equal(decoded_size, sizeof(payload));
    zassert_false(Deserializes(*decoded));
}

ZTEST(ui_configuration_parser, test_rejects_legacy_text_ui_property_keys) {
    const uint8_t payload[] = {
        0x84, 0x01, 0x00, 0xa1, 0x68, 'W', 'I', 'D', 'T', 'H', '_', 'P', 'X', 0x18, 0x50, 0x80
    };
    auto decoded = make_unique_pmr<CborUiConfig>(Mrm::GetDefaultPmr());
    size_t decoded_size = 0;
    zassert_not_equal(cbor_decode_CborUiConfig(payload, sizeof(payload), decoded.get(), &decoded_size), 0);
}

ZTEST(ui_configuration_parser, test_rejects_undefined_ui_property_ids) {
    const std::array<uint32_t, 5> invalid_ids = {
        static_cast<uint32_t>(UiPropertyType::NONE), static_cast<uint32_t>(UiPropertyType::COUNT),
        9999, 0x10000U, UINT32_MAX
    };
    UiConfigurationCborParser parser;
    auto configuration = ui_configuration_parser_GetTestUiConfiguration();
    auto serialized = parser.Serialize(*configuration);
    serialized->properties_present = true;
    auto& property = serialized->properties.CborPropertyValueType_m.emplace_back();
    property.CborPropertyValueType_m.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_c;
    property.CborPropertyValueType_m.value = 0;
    for(auto id : invalid_ids) {
        property.CborPropertyValueType_m_key = id;
        zassert_false(Deserializes(*serialized), "Accepted undefined UI property ID %u.", id);
    }
}

ZTEST(ui_configuration_parser, test_serialize_rejects_undefined_ui_properties) {
    UiConfigurationCborParser parser;
    auto configuration = ui_configuration_parser_GetTestUiConfiguration();
    configuration->properties[static_cast<UiPropertyType>(29)] = 80;

    bool threw = false;
    try {
        parser.Serialize(*configuration);
    } catch(const std::invalid_argument&) {
        threw = true;
    }

    zassert_true(threw);
}
