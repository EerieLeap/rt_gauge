#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <zephyr/ztest.h>

#include <eerie_memory.hpp>

#include "subsys/event_bus/event_channel.h"

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/string/string_helpers.h"

#include "domain/sensor_domain/event_bus/sensor_events_channel.h"
#include "domain/settings_domain/event_bus/settings_events_channel.h"
#include "domain/ui_domain/models/property_binding.h"
#include "domain/ui_domain/models/widget_configuration.h"

#include "event_bus/event_channel_id.h"
#include "event_bus/event_channels.h"

#include "views/utilitites/frame.h"
#include "views/widgets/widget_base.h"

#include "views_test_support.h"

using namespace eerie_memory;

using eerie_leap::domain::sensor_domain::event_bus::SensorEventsChannel;
using eerie_leap::domain::sensor_domain::event_bus::SensorEventType;
using eerie_leap::domain::sensor_domain::event_bus::SensorPayloadType;
using eerie_leap::domain::settings_domain::event_bus::SettingsEventsChannel;
using eerie_leap::domain::settings_domain::event_bus::SettingsEventType;
using eerie_leap::domain::settings_domain::event_bus::SettingsPayloadType;
using eerie_leap::domain::ui_domain::models::PropertyBinding;
using eerie_leap::domain::ui_domain::models::PropertyBindingDirection;
using eerie_leap::domain::ui_domain::models::WidgetConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetPropertyType;
using eerie_leap::domain::ui_domain::models::WidgetType;
using eerie_leap::event_bus::EventChannelId;
using eerie_leap::event_bus::InitializeEventChannels;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::utilities::string::StringHelpers;
using eerie_leap::utilities::type::ConfigValue;
using eerie_leap::utilities::type::ConfigValueAs;
using eerie_leap::views::themes::ITheme;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::widgets::PropertyChangeEffect;
using eerie_leap::views::widgets::WidgetBase;
using eerie_leap::views::widgets::WidgetContext;
using eerie_leap::views::widgets::WidgetPropertyStore;
using views_test::CleanTestDisplay;
using views_test::EnsureTestDisplay;

namespace {

constexpr const char* SENSOR_ID = "sensor_1";
constexpr const char* OTHER_SENSOR_ID = "sensor_2";
constexpr const char* SETTING_ID = "display.brightness";
constexpr int DISPATCH_TIMEOUT_MS = 1000;
constexpr int NO_DISPATCH_TIMEOUT_MS = 200;

constexpr WidgetPropertyType color_properties[] = {
    WidgetPropertyType::COLOR_PRIMARY_ACTIVE, WidgetPropertyType::COLOR_PRIMARY_INACTIVE,
    WidgetPropertyType::COLOR_SECONDARY_ACTIVE, WidgetPropertyType::COLOR_SECONDARY_INACTIVE,
    WidgetPropertyType::COLOR_TERTIARY_ACTIVE, WidgetPropertyType::COLOR_TERTIARY_INACTIVE
};

std::shared_ptr<Frame> MakeRoot() {
    return std::make_shared<Frame>(Frame::CreateWrapped()
        .SetWidth(100, false)
        .SetHeight(100, false)
        .Build());
}

// A widget with a property surface a test can watch, so the binding path is observed directly
// rather than through whatever a real widget happens to draw.
class ProbeWidget : public WidgetBase {
public:
    ProbeWidget(uint32_t id, std::shared_ptr<Frame> parent)
        : WidgetBase(id, std::move(parent), WidgetContext { }) { }

    WidgetType GetType() const override { return WidgetType::BasicIcon; }

    ConfigValue Read(WidgetPropertyType type) const { return properties_->Get(type); }
    double ReadNumber(WidgetPropertyType type) const { return ConfigValueAs<double>(Read(type), -1); }
    void WriteLocal(WidgetPropertyType type, const ConfigValue& value) { SetPropertyLocal(type, value); }

    std::vector<WidgetPropertyType> notified;

private:
    int DoRender() override { return 0; }
    int ApplyTheme(const ITheme&) override { return 0; }

    void RegisterProperties(WidgetPropertyStore& store) const override {
        WidgetBase::RegisterProperties(store);

        store.Register(WidgetPropertyType::VALUE, ConfigValue { 0.0 }, PropertyChangeEffect::None);
        store.Register(WidgetPropertyType::LABEL, ConfigValue { std::pmr::string { } }, PropertyChangeEffect::None);
        store.Register(WidgetPropertyType::OPACITY, ConfigValue { 255 }, PropertyChangeEffect::None);
        for(auto type : color_properties)
            store.Register(type, ConfigValue { std::pmr::string { } }, PropertyChangeEffect::None);
    }

    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override {
        notified.push_back(type);

        WidgetBase::OnPropertyChanged(type, value);
    }
};

std::shared_ptr<WidgetConfiguration> MakeConfiguration() {
    auto configuration = make_shared_pmr<WidgetConfiguration>(Mrm::GetExtPmr());
    configuration->type = WidgetType::BasicIcon;
    configuration->id = 1;

    return configuration;
}

PropertyBinding SensorBinding(WidgetPropertyType target, const char* sensor_id) {
    return PropertyBinding {
        .target = target,
        .channel = EventChannelId::Sensors,
        .event_type = std::to_underlying(SensorEventType::DataUpdated),
        .payload_key = std::to_underlying(SensorPayloadType::Value),
        .selector_key = std::to_underlying(SensorPayloadType::SensorId),
        .selector_value = std::pmr::string(sensor_id, Mrm::GetExtPmr())
    };
}

PropertyBinding SettingBinding(PropertyBindingDirection direction) {
    return PropertyBinding {
        .target = WidgetPropertyType::VALUE,
        .channel = EventChannelId::Settings,
        .event_type = std::to_underlying(SettingsEventType::Changed),
        .payload_key = std::to_underlying(SettingsPayloadType::Value),
        .direction = direction,
        .outbound_event_type = std::to_underlying(SettingsEventType::ChangeRequested),
        .selector_key = std::to_underlying(SettingsPayloadType::SettingId),
        .selector_value = std::pmr::string(SETTING_ID, Mrm::GetExtPmr())
    };
}

// Published synchronously so a test observes the result without waiting on the bus worker.
void PublishSensor(const char* sensor_id, const EventData& value) {
    SensorEventsChannel::GetInstance().Publish({
        .source_id = 0,
        .type = SensorEventType::DataUpdated,
        .payload = {
            { SensorPayloadType::SensorId, StringHelpers::GetHash(sensor_id) },
            { SensorPayloadType::Value, value }
        }
    });
}

void PublishSettingChanged(float value) {
    SettingsEventsChannel::GetInstance().Publish({
        .source_id = 0,
        .type = SettingsEventType::Changed,
        .payload = {
            { SettingsPayloadType::SettingId, StringHelpers::GetHash(SETTING_ID) },
            { SettingsPayloadType::Value, value }
        }
    });
}

// Watches what a widget publishes on an outbound binding.
class ChangeRequestProbe {
public:
    ChangeRequestProbe() : state_(std::make_shared<State>()) {
        k_sem_init(&state_->delivered, 0, K_SEM_MAX_LIMIT);

        subscription_ = CreateScopedSubscription(
            SettingsEventsChannel::GetInstance(),
            SettingsEventType::ChangeRequested,
            [state = state_](const SettingsEventsChannel::EventMessage& event) {
                if(auto it = event.payload.find(SettingsPayloadType::Value); it != event.payload.end())
                    state->value = std::get<float>(it->second);

                if(auto it = event.payload.find(SettingsPayloadType::SettingId); it != event.payload.end())
                    state->setting_id = std::get<uint32_t>(it->second);

                ++state->calls;
                k_sem_give(&state->delivered);
            });
    }

    bool WaitForRequest() const { return k_sem_take(&state_->delivered, K_MSEC(DISPATCH_TIMEOUT_MS)) == 0; }
    bool WaitForNoRequest() const { return k_sem_take(&state_->delivered, K_MSEC(NO_DISPATCH_TIMEOUT_MS)) != 0; }

    [[nodiscard]] int Calls() const { return state_->calls; }
    [[nodiscard]] std::optional<float> Value() const { return state_->value; }
    [[nodiscard]] std::optional<uint32_t> SettingId() const { return state_->setting_id; }

private:
    struct State {
        int calls = 0;
        std::optional<float> value;
        std::optional<uint32_t> setting_id;
        k_sem delivered{};
    };

    std::shared_ptr<State> state_;
    AnySubscription subscription_;
};

// Configure -> render -> activate, which is the state a widget is in while events flow.
std::unique_ptr<ProbeWidget> MakeActiveWidget(std::shared_ptr<WidgetConfiguration> configuration) {
    auto widget = std::make_unique<ProbeWidget>(1, MakeRoot());

    widget->Configure(std::move(configuration));
    zassert_equal(widget->Render(), 0);
    widget->OnActivated();
    widget->notified.clear();

    return widget;
}

void* SetUp() {
    EnsureTestDisplay();

    // The real wiring: both buses up and every channel in the registry, which is what a binding
    // resolves against.
    InitializeEventChannels();

    return nullptr;
}

} // namespace

ZTEST_SUITE(widget_bindings, NULL, SetUp, NULL, CleanTestDisplay, NULL);

ZTEST(widget_bindings, test_color_store_rejects_invalid_updates_and_accepts_reset) {
    WidgetPropertyStore store;
    const ConfigValue invalid_values[] = {
        {}, 0, 255, 128.0, true, false, std::pmr::string("#3366FF"), std::pmr::string("#3366FF8G"),
        std::pmr::string("#3366FF80\0", 10)
    };
    for(auto type : color_properties) {
        store.Register(type, std::pmr::string{}, PropertyChangeEffect::None);
        zassert_true(std::get<std::pmr::string>(store.Get(type)).empty());
        for(auto text : { "#3366fF80", "#00000000", "#FFFFFFFF" }) {
            ConfigValue color = std::pmr::string(text, Mrm::GetExtPmr());
            zassert_true(store.Set(type, color));
            for(const auto& value : invalid_values) {
                zassert_false(store.Set(type, value));
                zassert_true(store.Get(type) == color);
            }
        }
        zassert_true(store.Set(type, std::pmr::string{}));
        zassert_true(std::get<std::pmr::string>(store.Get(type)).empty());
    }
}

ZTEST(widget_bindings, test_opacity_store_rejects_invalid_updates_without_changing_value) {
    WidgetPropertyStore store;
    store.Register(WidgetPropertyType::OPACITY, 255, PropertyChangeEffect::None);
    zassert_equal(std::get<int>(store.Get(WidgetPropertyType::OPACITY)), 255);
    const ConfigValue invalid_values[] = {
        {}, -1, 256, INT32_MIN, INT32_MAX, 0.0, 128.5, 255.0, true, false, std::pmr::string("128")
    };
    for(int opacity : { 0, 128, 255 }) {
        zassert_true(store.Set(WidgetPropertyType::OPACITY, opacity));
        for(const auto& value : invalid_values) {
            zassert_false(store.Set(WidgetPropertyType::OPACITY, value));
            zassert_equal(std::get<int>(store.Get(WidgetPropertyType::OPACITY)), opacity);
        }
    }
}

ZTEST(widget_bindings, test_color_bindings_preserve_exact_rgba_and_reject_invalid_input) {
    const EventData invalid_values[] = {
        0, uint32_t{255}, 128.0F, true, false, std::string("#3366FF"), std::string("3366FF80"),
        std::string("#3366FF8G"), std::string("#3366FF80\0", 10)
    };
    for(auto type : color_properties) {
        auto configuration = MakeConfiguration();
        configuration->bindings.push_back(SensorBinding(type, SENSOR_ID));
        auto widget = MakeActiveWidget(std::move(configuration));
        for(auto text : { "#3366fF80", "#00000000", "#FFFFFFFF", "" }) {
            PublishSensor(SENSOR_ID, std::string(text));
            zassert_true(std::get<std::pmr::string>(widget->Read(type)) == text);
            zassert_equal(widget->notified.size(), 1U);
            widget->notified.clear();
            for(const auto& value : invalid_values) {
                PublishSensor(SENSOR_ID, value);
                zassert_true(std::get<std::pmr::string>(widget->Read(type)) == text);
                zassert_true(widget->notified.empty());
            }
        }
    }
}

ZTEST(widget_bindings, test_opacity_bindings_validate_before_numeric_coercion) {
    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::OPACITY, SENSOR_ID));
    auto widget = MakeActiveWidget(std::move(configuration));
    zassert_equal(std::get<int>(widget->Read(WidgetPropertyType::OPACITY)), 255);
    const EventData invalid_values[] = {
        -1, 256, INT32_MIN, INT32_MAX, uint32_t{256}, UINT32_MAX,
        0.0F, 128.5F, 255.0F, true, false, std::string("128")
    };
    const EventData valid_values[] = { 0, 128, 255, uint32_t{0}, uint32_t{128}, uint32_t{255} };
    for(const auto& opacity : valid_values) {
        PublishSensor(SENSOR_ID, opacity);
        int expected = std::holds_alternative<int>(opacity)
            ? std::get<int>(opacity) : static_cast<int>(std::get<uint32_t>(opacity));
        zassert_equal(std::get<int>(widget->Read(WidgetPropertyType::OPACITY)), expected);
        zassert_equal(widget->notified.size(), 1U);
        widget->notified.clear();
        for(const auto& value : invalid_values) {
            PublishSensor(SENSOR_ID, value);
            zassert_equal(std::get<int>(widget->Read(WidgetPropertyType::OPACITY)), expected);
            zassert_true(widget->notified.empty());
        }
    }
}

ZTEST(widget_bindings, test_a_binding_delivers_an_event_value_to_its_property) {
    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));

    auto widget = MakeActiveWidget(std::move(configuration));

    PublishSensor(SENSOR_ID, 42.5F);

    zassert_within(widget->ReadNumber(WidgetPropertyType::VALUE), 42.5, 0.001);
    zassert_equal(widget->notified.size(), 1U);
    zassert_equal(widget->notified.front(), WidgetPropertyType::VALUE);
}

// The persisted literal is the value until an event lands, which is what makes a binding optional.
ZTEST(widget_bindings, test_an_unbound_property_keeps_its_configured_value) {
    auto configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::VALUE] = 7.0;

    auto widget = MakeActiveWidget(std::move(configuration));

    PublishSensor(SENSOR_ID, 42.5F);

    zassert_within(widget->ReadNumber(WidgetPropertyType::VALUE), 7.0, 0.001);
    zassert_true(widget->notified.empty());
}

ZTEST(widget_bindings, test_one_event_can_drive_two_properties) {
    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::IS_VISIBLE, SENSOR_ID));

    auto widget = MakeActiveWidget(std::move(configuration));

    PublishSensor(SENSOR_ID, 1.0F);

    zassert_within(widget->ReadNumber(WidgetPropertyType::VALUE), 1.0, 0.001);
    zassert_true(widget->IsVisible());
    zassert_equal(widget->notified.size(), 2U);

    // The float coerces to the alternative each property was registered with, not the one the
    // publisher happened to send.
    PublishSensor(SENSOR_ID, 0.0F);

    zassert_false(widget->IsVisible());
}

ZTEST(widget_bindings, test_a_selector_mismatch_drops_the_event) {
    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));

    auto widget = MakeActiveWidget(std::move(configuration));

    PublishSensor(OTHER_SENSOR_ID, 42.5F);

    zassert_within(widget->ReadNumber(WidgetPropertyType::VALUE), 0.0, 0.001);
    zassert_true(widget->notified.empty());
}

// Configuration outlives the code that reads it, so neither of these may be fatal.
ZTEST(widget_bindings, test_a_binding_to_an_unsupported_property_is_dropped) {
    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::START_ANGLE, SENSOR_ID));

    auto widget = MakeActiveWidget(std::move(configuration));

    PublishSensor(SENSOR_ID, 42.5F);

    zassert_true(widget->notified.empty());
}

ZTEST(widget_bindings, test_a_binding_to_an_unregistered_channel_is_inert) {
    auto configuration = MakeConfiguration();

    auto binding = SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID);
    binding.channel = EventChannelId::None;
    configuration->bindings.push_back(std::move(binding));

    auto widget = MakeActiveWidget(std::move(configuration));

    PublishSensor(SENSOR_ID, 42.5F);

    zassert_within(widget->ReadNumber(WidgetPropertyType::VALUE), 0.0, 0.001);
    zassert_true(widget->notified.empty());
}

// The whole of the echo suppression rule: without it this inbound value would publish a request,
// which produces another inbound value, without bound.
ZTEST(widget_bindings, test_an_inbound_value_does_not_publish) {
    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(SettingBinding(PropertyBindingDirection::InOut));

    auto widget = MakeActiveWidget(std::move(configuration));

    ChangeRequestProbe probe;

    PublishSettingChanged(80.0F);

    zassert_within(widget->ReadNumber(WidgetPropertyType::VALUE), 80.0, 0.001);
    zassert_true(probe.WaitForNoRequest(), "An inbound value must not publish.");
    zassert_equal(probe.Calls(), 0);
}

ZTEST(widget_bindings, test_user_input_publishes_on_an_outbound_binding) {
    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(SettingBinding(PropertyBindingDirection::InOut));

    auto widget = MakeActiveWidget(std::move(configuration));

    ChangeRequestProbe probe;

    widget->WriteLocal(WidgetPropertyType::VALUE, ConfigValue { 55.0 });

    zassert_true(probe.WaitForRequest(), "Expected a change request.");
    zassert_equal(probe.Calls(), 1);
    zassert_true(probe.Value().has_value());
    zassert_within(*probe.Value(), 55.0F, 0.001F);
    zassert_true(probe.SettingId().has_value());
    zassert_equal(*probe.SettingId(), StringHelpers::GetHash(SETTING_ID));
}

// The model stays live while the view is skipped, so a widget whose group is hidden does not need
// anything re-announced when it comes back.
ZTEST(widget_bindings, test_a_deactivated_widget_tracks_the_store_and_catches_up_on_activation) {
    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));

    auto widget = MakeActiveWidget(std::move(configuration));

    widget->OnDeactivated();

    PublishSensor(SENSOR_ID, 42.5F);

    zassert_within(widget->ReadNumber(WidgetPropertyType::VALUE), 42.5, 0.001);
    zassert_true(widget->notified.empty(), "A hidden widget must not render.");

    widget->OnActivated();

    zassert_false(widget->notified.empty(), "Activation must replay the store.");
    zassert_within(widget->ReadNumber(WidgetPropertyType::VALUE), 42.5, 0.001);
}
