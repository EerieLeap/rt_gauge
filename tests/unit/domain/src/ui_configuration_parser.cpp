#include <memory>
#include <stdexcept>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "configuration/services/cbor_configuration_service.h"

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
    widget1->properties[WidgetProperty::GetTypeName(WidgetPropertyType::MIN_VALUE)] = 0;
    widget1->properties[WidgetProperty::GetTypeName(WidgetPropertyType::MAX_VALUE)] = 100;
    widget1->properties[WidgetProperty::GetTypeName(WidgetPropertyType::LABEL)] = "sensor_1";

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
    widget2->properties[WidgetProperty::GetTypeName(WidgetPropertyType::IS_VISIBLE)] = false;
    widget2->properties[WidgetProperty::GetTypeName(WidgetPropertyType::MIN_VALUE)] = 0;
    widget2->properties[WidgetProperty::GetTypeName(WidgetPropertyType::MAX_VALUE)] = 100;
    widget2->properties[WidgetProperty::GetTypeName(WidgetPropertyType::LABEL)] = "sensor_1";

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
    widget3->properties[WidgetProperty::GetTypeName(WidgetPropertyType::MIN_VALUE)] = 0;
    widget3->properties[WidgetProperty::GetTypeName(WidgetPropertyType::MAX_VALUE)] = 100;
    widget3->properties[WidgetProperty::GetTypeName(WidgetPropertyType::LABEL)] = "sensor_1";
    widget3->properties[WidgetProperty::GetTypeName(WidgetPropertyType::CHART_POINT_COUNT)] = 35;
    widget3->properties[WidgetProperty::GetTypeName(WidgetPropertyType::CHART_TYPE)] = static_cast<std::uint16_t>(HorizontalChartIndicatorType::Line);
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
    widget.properties["ICON_TYPE"] = static_cast<int>(IconType::TriangleRight);
    widget.properties["WIDTH_PX"] = 80;
    widget.properties["HEIGHT_PX"] = 40;
    widget.properties["STROKE_PX"] = 3;
    widget.properties["CORNER_RAD_PX"] = 8;
    widget.properties["FILL_MODE"] = static_cast<int>(WidgetFillMode::Outline);
    widget.properties["DIRECTION"] = static_cast<int>(WidgetDirection::TopToBottom);
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
