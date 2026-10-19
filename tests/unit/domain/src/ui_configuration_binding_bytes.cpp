#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory_resource>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <eerie_memory.hpp>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_serializer.h"
#include "configuration/cbor/cbor_ui_config/cbor_ui_config.h"
#include "utilities/memory/memory_resource_manager.h"

#include "domain/logging_domain/event_bus/logging_event_type.h"
#include "domain/logging_domain/event_bus/logging_payload_type.h"
#include "domain/sensor_domain/event_bus/sensor_event_type.h"
#include "domain/sensor_domain/event_bus/sensor_payload_type.h"
#include "domain/settings_domain/event_bus/settings_event_type.h"
#include "domain/settings_domain/event_bus/settings_payload_type.h"
#include "domain/ui_domain/event_bus/navigation_event_type.h"
#include "domain/ui_domain/event_bus/navigation_payload_type.h"

#include "domain/ui_domain/configuration/parsers/ui_configuration_cbor_parser.h"
#include "domain/ui_domain/models/property_binding.h"
#include "domain/ui_domain/models/ui_configuration.h"
#include "domain/ui_domain/models/widget_property.h"
#include "domain/ui_domain/models/widget_type.h"

#include "subsys/event_bus/event_channel_id.h"

using namespace eerie_memory;
using namespace eerie_leap::domain::ui_domain::models;

using eerie_leap::configuration::cbor::CborSerializer;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventType;
using eerie_leap::domain::logging_domain::event_bus::LoggingPayloadType;
using eerie_leap::domain::sensor_domain::event_bus::SensorEventType;
using eerie_leap::domain::sensor_domain::event_bus::SensorPayloadType;
using eerie_leap::domain::settings_domain::event_bus::SettingsEventType;
using eerie_leap::domain::settings_domain::event_bus::SettingsPayloadType;
using eerie_leap::domain::ui_domain::configuration::parsers::UiConfigurationCborParser;
using eerie_leap::domain::ui_domain::event_bus::NavigationEventType;
using eerie_leap::domain::ui_domain::event_bus::NavigationPayloadType;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::utilities::memory::Mrm;

namespace {

// A stored UI configuration with one binding of each shape, captured once from the encoder before
// EventBinding moved to rt_core. A mismatch means stored UI configurations no longer read the same:
// fix the code, never the bytes.
//
// zcbor without ZCBOR_CANONICAL, as the app builds it, opens every list with 0x9f and closes it with
// 0xff. A binding record is [target, channel, event_type, payload_key, direction,
// outbound_event_type, has_selector, selector_key, selector_value]; a target of 24 or more takes
// two bytes (0x18 nn), and an absent selector still encodes the placeholder 0.
// clang-format off: each row holds the bytes its comment describes, and the longer rows pass the column limit.
const uint8_t STORED_CONFIGURATION[] = {
    0x9f, 0x01, 0x01, 0x9f,                                     // [version 1, active group 1, screens [
    0x9f, 0x01, 0x01, 0x02, 0x00, 0xf5, 0xf4,                   //   [id 1, group 1, Gauge, z 0, visible, not overlay,
    0x9f, 0xf4, 0x01, 0x01, 0x00, 0xff, 0x9f,                   //    grid [no snap, 1, 1, 0], widgets [
    0x9f, 0x1a, 0x00, 0x02, 0x00, 0x66, 0x01,                   //     [IndicatorDigital, id 1,
    0x9f, 0x00, 0x00, 0xff, 0x9f, 0x01, 0x01, 0xff, 0x00, 0x9f, //      position [0, 0], size [1, 1], z 0, bindings [
    0x9f, 0x18, 0x1b, 0x01, 0x00, 0x01, 0x00, 0x00, 0xf5, 0x00, // VALUE, Sensors, DataUpdated, Value, In, 0, SensorId
    0x68, 0x73, 0x65, 0x6e, 0x73, 0x6f, 0x72, 0x5f, 0x31, 0xff, //   "sensor_1"
    0x9f, 0x18, 0x1a, 0x02, 0x00, 0x00, 0x00, 0x00, 0xf4, 0x00, // IS_VISIBLE, Logging, StatusUpdated, IsActive, In, 0, none
    0x00, 0xff,                                                 //   placeholder 0
    0x9f, 0x03, 0x01, 0x00, 0x01, 0x00, 0x00, 0xf5, 0x00,       // MIN_VALUE, Sensors, DataUpdated, Value, In, 0, SensorId
    0x07, 0xff,                                                 //   7
    0x9f, 0x18, 0x22, 0x02, 0x00, 0x01, 0x00, 0x00, 0xf5, 0x00, // OPACITY, Logging, StatusUpdated, Reason, In, 0, IsActive
    0xf5, 0xff,                                                 //   true
    0x9f, 0x04, 0x04, 0x00, 0x01, 0x02, 0x02, 0xf5, 0x00,       // MAX_VALUE, Settings, Changed, Value, InOut, ChangeRequested, SettingId
    0x6a, 0x62, 0x72, 0x69, 0x67, 0x68, 0x74, 0x6e, 0x65, 0x73, //   "brightness"
    0x73, 0xff,
    0x9f, 0x18, 0x19, 0x03, 0x00, 0x01, 0x01, 0x00, 0xf5, 0x00, // TARGET_SCREEN_GROUP, Navigation, Changed, TargetGroupId, Out, Changed, Action
    0x21, 0xff,                                                 //   -2
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff                          // bindings, widget, widgets, screen, screens, configuration
};
// clang-format on

BUILD_ASSERT(
    !IS_ENABLED(CONFIG_ZCBOR_CANONICAL),
    "The stored bytes are those of non-canonical zcbor, as the app builds it."
);

PropertyBinding MakeBinding(
    WidgetPropertyType target,
    EventChannelId channel,
    uint32_t event_type,
    uint32_t payload_key,
    PropertyBindingDirection direction,
    uint32_t outbound_event_type,
    uint32_t selector_key,
    ConfigValue selector_value
) {

    return PropertyBinding{ .target = target,
        .event = { .channel = channel,
            .event_type = event_type,
            .payload_key = payload_key,
            .selector_key = selector_key,
            .selector_value = std::move(selector_value) },
        .direction = direction,
        .outbound_event_type = outbound_event_type };
}

// No properties at any level, since property maps encode in unordered_map order, and one widget,
// so ordering by z-index on load cannot move anything.
PmrUniquePtr<UiConfiguration> MakeStoredConfiguration() {
    auto configuration = make_unique_pmr<UiConfiguration>(Mrm::GetDefaultPmr());
    configuration->active_screen_group_id = 1;

    auto screen = make_shared_pmr<ScreenConfiguration>(Mrm::GetDefaultPmr());
    screen->id = 1;
    screen->screen_group_id = 1;
    screen->type = ScreenType::Gauge;
    screen->z_index = 0;
    screen->is_visible = true;
    screen->is_overlay = false;
    screen->grid = { .snap_enabled = false, .width = 1, .height = 1, .spacing_px = 0 };

    auto widget = make_shared_pmr<WidgetConfiguration>(Mrm::GetDefaultPmr());
    widget->type = WidgetType::IndicatorDigital;
    widget->id = 1;
    widget->position_grid = { 0, 0 };
    widget->size_grid = { 1, 1 };
    widget->z_index = 0;

    auto& bindings = widget->bindings;

    // A sensor selected by its text id.
    bindings.push_back(MakeBinding(
        WidgetPropertyType::VALUE, EventChannelId::Sensors, std::to_underlying(SensorEventType::DataUpdated),
        std::to_underlying(SensorPayloadType::Value), PropertyBindingDirection::In, 0,
        std::to_underlying(SensorPayloadType::SensorId), std::pmr::string("sensor_1")
    ));

    // Unconditional.
    bindings.push_back(MakeBinding(
        WidgetPropertyType::IS_VISIBLE, EventChannelId::Logging, std::to_underlying(LoggingEventType::StatusUpdated),
        std::to_underlying(LoggingPayloadType::IsActive), PropertyBindingDirection::In, 0, 0, ConfigValue{}
    ));

    // An integer selector.
    bindings.push_back(MakeBinding(
        WidgetPropertyType::MIN_VALUE, EventChannelId::Sensors, std::to_underlying(SensorEventType::DataUpdated),
        std::to_underlying(SensorPayloadType::Value), PropertyBindingDirection::In, 0,
        std::to_underlying(SensorPayloadType::SensorId), 7
    ));

    // A bool selector.
    bindings.push_back(MakeBinding(
        WidgetPropertyType::OPACITY, EventChannelId::Logging, std::to_underlying(LoggingEventType::StatusUpdated),
        std::to_underlying(LoggingPayloadType::Reason), PropertyBindingDirection::In, 0,
        std::to_underlying(LoggingPayloadType::IsActive), true
    ));

    // Two-way, with the event the widget publishes when the user moves it.
    bindings.push_back(MakeBinding(
        WidgetPropertyType::MAX_VALUE, EventChannelId::Settings, std::to_underlying(SettingsEventType::Changed),
        std::to_underlying(SettingsPayloadType::Value), PropertyBindingDirection::InOut,
        std::to_underlying(SettingsEventType::ChangeRequested), std::to_underlying(SettingsPayloadType::SettingId),
        std::pmr::string("brightness")
    ));

    // Outbound only, with a negative integer selector.
    bindings.push_back(MakeBinding(
        WidgetPropertyType::TARGET_SCREEN_GROUP, EventChannelId::Navigation,
        std::to_underlying(NavigationEventType::Changed), std::to_underlying(NavigationPayloadType::TargetGroupId),
        PropertyBindingDirection::Out, std::to_underlying(NavigationEventType::Changed),
        std::to_underlying(NavigationPayloadType::Action), -2
    ));

    screen->AddWidget(std::move(widget));
    configuration->screen_configurations.push_back(std::move(screen));

    return configuration;
}

// The stored bytes with the integer selector 7 of the MIN_VALUE binding replaced by @p selector.
std::vector<uint8_t> WithSelectorBytes(std::span<const uint8_t> selector) {
    constexpr uint8_t MARK[] = { 0x9f, 0x03, 0x01, 0x00, 0x01, 0x00, 0x00, 0xf5, 0x00 };

    std::vector<uint8_t> bytes(std::begin(STORED_CONFIGURATION), std::end(STORED_CONFIGURATION));
    const auto mark = std::ranges::search(bytes, MARK).begin();
    const auto value = mark + sizeof(MARK);
    bytes.erase(value);
    bytes.insert(value, selector.begin(), selector.end());

    return bytes;
}

// For diagnosis only: the array a mismatch would have to be compared against.
void PrintBytes(std::span<const uint8_t> bytes) {
    printk("Encoded %u bytes:\n", static_cast<unsigned>(bytes.size()));

    for(size_t i = 0; i < bytes.size(); ++i)
        printk("0x%02x,%s", bytes[i], (i % 12 == 11 || i + 1 == bytes.size()) ? "\n" : " ");
}

} // namespace

ZTEST_SUITE(ui_configuration_binding_bytes, NULL, NULL, NULL, NULL, NULL);

ZTEST(ui_configuration_binding_bytes, test_bindings_encode_to_the_stored_bytes) {
    auto configuration = MakeStoredConfiguration();
    auto cbor_configuration = UiConfigurationCborParser().Serialize(*configuration);
    auto bytes = CborSerializer<CborUiConfig>().Serialize(*cbor_configuration);

    const bool matches = bytes.size() == sizeof(STORED_CONFIGURATION)
        && std::memcmp(bytes.data(), STORED_CONFIGURATION, bytes.size()) == 0;

    if(!matches)
        PrintBytes(bytes);

    zassert_true(matches, "The encoder no longer writes the stored bytes.");
}

ZTEST(ui_configuration_binding_bytes, test_stored_bytes_decode_to_the_same_bindings) {
    auto cbor_configuration = CborSerializer<CborUiConfig>().Deserialize(STORED_CONFIGURATION);
    zassert_not_null(cbor_configuration.get());

    auto configuration = UiConfigurationCborParser().Deserialize(Mrm::GetDefaultPmr(), *cbor_configuration);

    zassert_equal(configuration->screen_configurations.size(), 1U);
    zassert_equal(configuration->screen_configurations[0]->widget_configurations.size(), 1U);

    const auto& bindings = configuration->screen_configurations[0]->widget_configurations[0]->bindings;
    zassert_equal(bindings.size(), 6U);

    // Channels as the literal values stored configurations carry.
    zassert_equal(bindings[0].target, WidgetPropertyType::VALUE);
    zassert_equal(std::to_underlying(bindings[0].event.channel), 1U);
    zassert_equal(bindings[0].event.event_type, std::to_underlying(SensorEventType::DataUpdated));
    zassert_equal(bindings[0].event.payload_key, std::to_underlying(SensorPayloadType::Value));
    zassert_equal(bindings[0].direction, PropertyBindingDirection::In);
    zassert_equal(bindings[0].outbound_event_type, 0U);
    zassert_true(bindings[0].event.HasSelector());
    zassert_equal(bindings[0].event.selector_key, std::to_underlying(SensorPayloadType::SensorId));
    zassert_true(std::get<std::pmr::string>(bindings[0].event.selector_value) == "sensor_1");

    zassert_equal(bindings[1].target, WidgetPropertyType::IS_VISIBLE);
    zassert_equal(std::to_underlying(bindings[1].event.channel), 2U);
    zassert_equal(bindings[1].event.event_type, std::to_underlying(LoggingEventType::StatusUpdated));
    zassert_equal(bindings[1].event.payload_key, std::to_underlying(LoggingPayloadType::IsActive));
    zassert_equal(bindings[1].direction, PropertyBindingDirection::In);
    zassert_equal(bindings[1].outbound_event_type, 0U);
    zassert_false(bindings[1].event.HasSelector());
    zassert_equal(bindings[1].event.selector_key, 0U);

    zassert_equal(bindings[2].target, WidgetPropertyType::MIN_VALUE);
    zassert_equal(std::to_underlying(bindings[2].event.channel), 1U);
    zassert_equal(bindings[2].event.event_type, std::to_underlying(SensorEventType::DataUpdated));
    zassert_equal(bindings[2].event.payload_key, std::to_underlying(SensorPayloadType::Value));
    zassert_equal(bindings[2].direction, PropertyBindingDirection::In);
    zassert_equal(bindings[2].outbound_event_type, 0U);
    zassert_true(bindings[2].event.HasSelector());
    zassert_equal(bindings[2].event.selector_key, std::to_underlying(SensorPayloadType::SensorId));
    zassert_equal(std::get<int>(bindings[2].event.selector_value), 7);

    zassert_equal(bindings[3].target, WidgetPropertyType::OPACITY);
    zassert_equal(std::to_underlying(bindings[3].event.channel), 2U);
    zassert_equal(bindings[3].event.event_type, std::to_underlying(LoggingEventType::StatusUpdated));
    zassert_equal(bindings[3].event.payload_key, std::to_underlying(LoggingPayloadType::Reason));
    zassert_equal(bindings[3].direction, PropertyBindingDirection::In);
    zassert_equal(bindings[3].outbound_event_type, 0U);
    zassert_true(bindings[3].event.HasSelector());
    zassert_equal(bindings[3].event.selector_key, std::to_underlying(LoggingPayloadType::IsActive));
    zassert_true(std::get<bool>(bindings[3].event.selector_value));

    zassert_equal(bindings[4].target, WidgetPropertyType::MAX_VALUE);
    zassert_equal(std::to_underlying(bindings[4].event.channel), 4U);
    zassert_equal(bindings[4].event.event_type, std::to_underlying(SettingsEventType::Changed));
    zassert_equal(bindings[4].event.payload_key, std::to_underlying(SettingsPayloadType::Value));
    zassert_equal(bindings[4].direction, PropertyBindingDirection::InOut);
    zassert_equal(bindings[4].outbound_event_type, std::to_underlying(SettingsEventType::ChangeRequested));
    zassert_true(bindings[4].event.HasSelector());
    zassert_equal(bindings[4].event.selector_key, std::to_underlying(SettingsPayloadType::SettingId));
    zassert_true(std::get<std::pmr::string>(bindings[4].event.selector_value) == "brightness");

    zassert_equal(bindings[5].target, WidgetPropertyType::TARGET_SCREEN_GROUP);
    zassert_equal(std::to_underlying(bindings[5].event.channel), 3U);
    zassert_equal(bindings[5].event.event_type, std::to_underlying(NavigationEventType::Changed));
    zassert_equal(bindings[5].event.payload_key, std::to_underlying(NavigationPayloadType::TargetGroupId));
    zassert_equal(bindings[5].direction, PropertyBindingDirection::Out);
    zassert_equal(bindings[5].outbound_event_type, std::to_underlying(NavigationEventType::Changed));
    zassert_true(bindings[5].event.HasSelector());
    zassert_equal(bindings[5].event.selector_key, std::to_underlying(NavigationPayloadType::Action));
    zassert_equal(std::get<int>(bindings[5].event.selector_value), -2);
}

ZTEST(ui_configuration_binding_bytes, test_selector_written_as_uint32_reads_as_the_same_32_bits) {
    constexpr uint8_t SELECTOR[] = { 0x1a, 0xff, 0xff, 0xff, 0xff }; // 4294967295
    const auto bytes = WithSelectorBytes(SELECTOR);

    auto cbor_configuration = CborSerializer<CborUiConfig>().Deserialize(bytes);
    zassert_not_null(cbor_configuration.get());

    auto configuration = UiConfigurationCborParser().Deserialize(Mrm::GetDefaultPmr(), *cbor_configuration);
    const auto& bindings = configuration->screen_configurations[0]->widget_configurations[0]->bindings;

    zassert_equal(std::get<int>(bindings[2].event.selector_value), -1);
}

ZTEST(ui_configuration_binding_bytes, test_selector_above_uint32_is_refused) {
    constexpr uint8_t SELECTOR[] = { 0x1b, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00 }; // 0x1_0000_0000
    const auto bytes = WithSelectorBytes(SELECTOR);

    zassert_is_null(CborSerializer<CborUiConfig>().Deserialize(bytes).get());
}

ZTEST(ui_configuration_binding_bytes, test_selector_below_int32_is_refused) {
    constexpr uint8_t SELECTOR[] = { 0x3b, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00 }; // -2147483649
    const auto bytes = WithSelectorBytes(SELECTOR);

    zassert_is_null(CborSerializer<CborUiConfig>().Deserialize(bytes).get());
}
