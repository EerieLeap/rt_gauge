#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <zephyr/ztest.h>

#include "utilities/string/string_helpers.h"

#include "subsys/event_bus/event_binding.h"
#include "subsys/event_bus/event_binding_subscription.h"
#include "subsys/event_bus/event_bus.h"
#include "subsys/event_bus/event_channel.h"
#include "subsys/event_bus/event_channel_id.h"
#include "subsys/event_bus/event_channel_registry.h"

#include "domain/logging_domain/event_bus/logging_events_channel.h"
#include "domain/settings_domain/event_bus/settings_events_channel.h"

using eerie_leap::domain::logging_domain::event_bus::LoggingEventsChannel;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventType;
using eerie_leap::domain::logging_domain::event_bus::LoggingPayloadType;
using eerie_leap::domain::settings_domain::event_bus::SettingsEventsChannel;
using eerie_leap::domain::settings_domain::event_bus::SettingsEventType;
using eerie_leap::domain::settings_domain::event_bus::SettingsPayloadType;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::event_bus::ErasedEventFilter;
using eerie_leap::subsys::event_bus::ErasedPayload;
using eerie_leap::subsys::event_bus::ErasedPayloadView;
using eerie_leap::subsys::event_bus::EventBinding;
using eerie_leap::subsys::event_bus::EventBus;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::EventChannelRegistry;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::subsys::event_bus::SubscribeEventBinding;
using eerie_leap::utilities::string::StringHelpers;

namespace {

constexpr int BUS_STACK_SIZE = 4096;

// Channels are inert until a bus drains them, so the suite owns one.
class TestEventBus : public EventBus {
public:
    TestEventBus() : EventBus("registry_test_bus", BUS_STACK_SIZE) {
        RegisterChannel(LoggingEventsChannel::GetInstance());
        RegisterChannel(SettingsEventsChannel::GetInstance());
    }
};

// Subscribes by channel id and raw keys through the registry and SubscribeErased(), never naming
// the enums. Widget bindings go through SubscribeEventBinding(), which the last case covers.
class ErasedProbe {
public:
    ErasedProbe(EventChannelId channel_id, uint32_t event_type, uint32_t payload_key,
        ErasedEventFilter selector = { }) {
        auto* channel = EventChannelRegistry::GetInstance().Find(channel_id);
        if(channel == nullptr)
            return;

        subscription_ = channel->SubscribeErased(
            event_type,
            selector,
            [this, payload_key](const ErasedPayloadView& view) {
                ++calls_;

                if(const auto* value = view.Find(payload_key))
                    last_value_ = *value;
            });
    }

    [[nodiscard]] bool IsSubscribed() const { return subscription_ != nullptr; }
    [[nodiscard]] int Calls() const { return calls_; }
    [[nodiscard]] const std::optional<EventData>& LastValue() const { return last_value_; }

private:
    AnySubscription subscription_;
    int calls_ = 0;
    std::optional<EventData> last_value_;
};

void PublishLoggingStatus(bool is_active) {
    LoggingEventsChannel::GetInstance().Publish({
        .source_id = 0x7E57,
        .type = LoggingEventType::StatusUpdated,
        .payload = { { LoggingPayloadType::IsActive, is_active } }
    });
}

void PublishSettingChanged(const char* setting_id, float value) {
    SettingsEventsChannel::GetInstance().Publish({
        .source_id = 0x7E57,
        .type = SettingsEventType::Changed,
        .payload = {
            { SettingsPayloadType::SettingId, StringHelpers::GetHash(setting_id) },
            { SettingsPayloadType::Value, value }
        }
    });
}

void* RegisterChannels() {
    static TestEventBus bus;

    auto& registry = EventChannelRegistry::GetInstance();
    registry.Register(EventChannelId::Logging, LoggingEventsChannel::GetInstance());
    registry.Register(EventChannelId::Settings, SettingsEventsChannel::GetInstance());

    return nullptr;
}

} // namespace

ZTEST_SUITE(event_channel_registry, NULL, RegisterChannels, NULL, NULL, NULL);

ZTEST(event_channel_registry, test_registered_channel_resolves) {
    auto* channel = EventChannelRegistry::GetInstance().Find(EventChannelId::Logging);

    zassert_not_null(channel);
    zassert_equal(channel, &LoggingEventsChannel::GetInstance());
}

// A binding persisted before its channel existed must be inert, not fatal.
ZTEST(event_channel_registry, test_unregistered_channel_resolves_to_null) {
    zassert_is_null(EventChannelRegistry::GetInstance().Find(EventChannelId::None));
    zassert_is_null(EventChannelRegistry::GetInstance().Find(EventChannelId::Sensors));
    zassert_is_null(EventChannelRegistry::GetInstance().Find(static_cast<EventChannelId>(99)));
}

ZTEST(event_channel_registry, test_erased_subscribe_delivers_the_payload_key) {
    ErasedProbe probe(
        EventChannelId::Logging,
        std::to_underlying(LoggingEventType::StatusUpdated),
        std::to_underlying(LoggingPayloadType::IsActive));

    zassert_true(probe.IsSubscribed());

    PublishLoggingStatus(true);

    zassert_equal(probe.Calls(), 1);
    zassert_true(probe.LastValue().has_value());
    zassert_true(std::get<bool>(*probe.LastValue()));
}

ZTEST(event_channel_registry, test_erased_subscribe_reports_a_missing_key) {
    ErasedProbe probe(
        EventChannelId::Logging,
        std::to_underlying(LoggingEventType::StatusUpdated),
        0xDEAD);

    PublishLoggingStatus(true);

    zassert_equal(probe.Calls(), 1);
    zassert_false(probe.LastValue().has_value());
}

// The selector a binding carries runs here, so an event meant for another subscriber never
// reaches this handler at all.
ZTEST(event_channel_registry, test_erased_filter_rejects_before_the_handler) {
    auto is_active = [](const ErasedPayloadView& view) {
        const auto* value = view.Find(std::to_underlying(LoggingPayloadType::IsActive));

        return value != nullptr && std::get<bool>(*value);
    };

    ErasedProbe probe(
        EventChannelId::Logging,
        std::to_underlying(LoggingEventType::StatusUpdated),
        std::to_underlying(LoggingPayloadType::IsActive),
        is_active);

    PublishLoggingStatus(false);
    zassert_equal(probe.Calls(), 0);

    PublishLoggingStatus(true);
    zassert_equal(probe.Calls(), 1);
}

ZTEST(event_channel_registry, test_erased_subscription_unsubscribes_on_destruction) {
    {
        ErasedProbe probe(
            EventChannelId::Logging,
            std::to_underlying(LoggingEventType::StatusUpdated),
            std::to_underlying(LoggingPayloadType::IsActive));

        PublishLoggingStatus(true);
        zassert_equal(probe.Calls(), 1);
    }

    ErasedProbe observer(
        EventChannelId::Logging,
        std::to_underlying(LoggingEventType::StatusUpdated),
        std::to_underlying(LoggingPayloadType::IsActive));

    PublishLoggingStatus(false);

    zassert_equal(observer.Calls(), 1);
}

// The write half of a binding: publish by raw event type and keys, read back through the enums.
ZTEST(event_channel_registry, test_erased_publish_reaches_a_typed_subscriber) {
    std::vector<uint32_t> setting_ids;
    std::optional<float> value;

    auto subscription = CreateScopedSubscription(
        SettingsEventsChannel::GetInstance(),
        SettingsEventType::ChangeRequested,
        [&](const SettingsEventsChannel::EventMessage& event) {
            if(auto it = event.payload.find(SettingsPayloadType::SettingId); it != event.payload.end())
                setting_ids.push_back(std::get<uint32_t>(it->second));

            if(auto it = event.payload.find(SettingsPayloadType::Value); it != event.payload.end())
                value = std::get<float>(it->second);
        });

    zassert_not_null(subscription.get());

    auto* channel = EventChannelRegistry::GetInstance().Find(EventChannelId::Settings);
    zassert_not_null(channel);

    std::array<std::pair<uint32_t, EventData>, 2> payload {
        std::pair { std::to_underlying(SettingsPayloadType::Value), EventData { 42.0F } },
        std::pair { std::to_underlying(SettingsPayloadType::SettingId), EventData { uint32_t { 7 } } }
    };

    channel->PublishErasedAsync(
        std::to_underlying(SettingsEventType::ChangeRequested), 0x7E57, ErasedPayload { payload });

    k_msleep(200);

    zassert_equal(setting_ids.size(), 1U);
    zassert_equal(setting_ids.front(), 7U);
    zassert_true(value.has_value());
    zassert_equal(*value, 42.0F);
}

// The path a widget's setting binding takes: rt_core's SubscribeEventBinding() resolving the
// gauge-only Settings channel, selected by the setting's text id.
ZTEST(event_channel_registry, test_a_binding_resolves_the_settings_channel_with_a_text_selector) {
    // Shared with the handler, so a failed assertion cannot leave it pointing into this frame.
    auto value = std::make_shared<std::optional<EventData>>();

    auto subscription = SubscribeEventBinding(
        EventBinding {
            .channel = EventChannelId::Settings,
            .event_type = std::to_underlying(SettingsEventType::Changed),
            .payload_key = std::to_underlying(SettingsPayloadType::Value),
            .selector_key = std::to_underlying(SettingsPayloadType::SettingId),
            .selector_value = std::pmr::string("display.brightness")
        },
        [value](const EventData& data) { *value = data; });

    zassert_not_null(subscription.get());

    PublishSettingChanged("display.contrast", 10.0F);
    zassert_false(value->has_value());

    PublishSettingChanged("display.brightness", 80.0F);
    zassert_true(value->has_value());
    zassert_equal(std::get<float>(**value), 80.0F);
}
