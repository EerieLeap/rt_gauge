#include <cstdint>
#include <limits>
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
#include "domain/ui_domain/models/animation.h"
#include "domain/ui_domain/lvgl_lock.h"
#include "domain/ui_domain/models/widget_configuration.h"

#include "event_bus/event_channel_id.h"
#include "event_bus/event_channels.h"

#include "views/screens/screen.h"
#include "views/screens/screen_group.h"
#include "views/utilitites/frame.h"
#include "views/widgets/controls/slider_control/slider_control.h"
#include "views/widgets/controls/toggle_control/toggle_control.h"
#include "views/widgets/indicators/bar_indicator/bar_indicator.h"
#include "views/widgets/indicators/digital_indicator/digital_indicator.h"
#include "views/widgets/indicators/horizontal_chart_indicator/horizontal_chart_indicator.h"
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
using eerie_leap::domain::ui_domain::models::Animation;
using views_test::CleanTestDisplay;
using views_test::EnsureTestDisplay;

namespace {

constexpr const char* SENSOR_ID = "sensor_1";
constexpr const char* OTHER_SENSOR_ID = "sensor_2";
constexpr const char* SETTING_ID = "display.brightness";
constexpr int DISPATCH_TIMEOUT_MS = 1000;
constexpr int NO_DISPATCH_TIMEOUT_MS = 200;

K_THREAD_STACK_DEFINE(publisher_stack, 4096);

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

    ~ProbeWidget() override { DetachDispatch(); }

    WidgetType GetType() const override { return WidgetType::BasicIcon; }

    ConfigValue Read(WidgetPropertyType type) const { return properties_->Get(type); }
    double ReadNumber(WidgetPropertyType type) const { return ConfigValueAs<double>(Read(type), -1); }
    void WriteLocal(WidgetPropertyType type, const ConfigValue& value) { SetPropertyLocal(type, value); }
    std::shared_ptr<WidgetPropertyStore> GetStore() const { return properties_; }

    std::vector<WidgetPropertyType> notified;

private:
    int DoRender() override { return 0; }
    int ApplyTheme(const ITheme&) override { return 0; }

    void RegisterProperties(WidgetPropertyStore& store) const override {
        WidgetBase::RegisterProperties(store);

        store.Register(WidgetPropertyType::VALUE, ConfigValue { 0.0 }, PropertyChangeEffect::None);
        store.Register(WidgetPropertyType::LABEL, ConfigValue { std::pmr::string { } }, PropertyChangeEffect::None);

        store.Register(WidgetPropertyType::ANIMATION_TYPE,
            static_cast<int>(Animation::DEFAULT_TYPE), PropertyChangeEffect::None);
        store.Register(WidgetPropertyType::IS_ANIMATION_ACTIVE, Animation::DEFAULT_ACTIVE, PropertyChangeEffect::None);
        store.Register(WidgetPropertyType::ANIMATION_DURATION_MS,
            Animation::DEFAULT_DURATION_MS, PropertyChangeEffect::None);
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
std::unique_ptr<ProbeWidget> MakeActiveWidget(
    std::shared_ptr<WidgetConfiguration> configuration, std::shared_ptr<Frame> parent = nullptr) {
    auto widget = std::make_unique<ProbeWidget>(1, parent != nullptr ? std::move(parent) : MakeRoot());

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

ZTEST(widget_bindings, test_digital_color_binding_applies_rgba_and_resets_without_rebuilding) {
    using eerie_leap::views::themes::ThemeManager;
    auto configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::COLOR_PRIMARY_ACTIVE] = std::pmr::string("#12345680");
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::COLOR_PRIMARY_ACTIVE, SENSOR_ID));
    eerie_leap::views::widgets::indicators::DigitalIndicator widget(1, MakeRoot(), WidgetContext{});
    widget.Configure(configuration);
    zassert_equal(widget.Render(), 0);
    widget.OnActivated();
    auto* label = widget.GetContainer()->GetChild()->GetObject();
    zassert_equal(lv_obj_get_style_text_opa(label, LV_PART_MAIN), 128);
    zassert_equal(lv_color_to_u32(lv_obj_get_style_text_color(label, LV_PART_MAIN)),
        lv_color_to_u32(lv_color_hex(0x123456)));
    PublishSensor(SENSOR_ID, std::string("#abcdef00"));
    zassert_equal(lv_obj_get_style_text_opa(label, LV_PART_MAIN), 0);
    PublishSensor(SENSOR_ID, std::string("#abcdef"));
    zassert_equal(lv_obj_get_style_text_opa(label, LV_PART_MAIN), 0);
    PublishSensor(SENSOR_ID, std::string(""));
    const auto fallback = ThemeManager::GetInstance().GetCurrentTheme().GetPrimaryColor();
    zassert_equal(lv_obj_get_style_text_opa(label, LV_PART_MAIN), fallback.ToLvOpa());
    zassert_equal(lv_color_to_u32(lv_obj_get_style_text_color(label, LV_PART_MAIN)),
        lv_color_to_u32(fallback.ToLvColor()));
    zassert_equal(widget.GetContainer()->GetChild()->GetObject(), label);
}

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

ZTEST(widget_bindings, test_color_cache_maps_properties_independently_of_registration_order) {
    using eerie_leap::views::utilities::LvglColor;
    struct ColorCase {
        WidgetPropertyType type;
        const char* text;
        uint32_t rgb;
        uint8_t alpha;
    };
    const ColorCase cases[] = {
        { WidgetPropertyType::COLOR_TERTIARY_INACTIVE, "#11223300", 0x112233, 0 },
        { WidgetPropertyType::COLOR_PRIMARY_ACTIVE, "#22334440", 0x223344, 64 },
        { WidgetPropertyType::COLOR_SECONDARY_INACTIVE, "#33445580", 0x334455, 128 },
        { WidgetPropertyType::COLOR_TERTIARY_ACTIVE, "#445566FF", 0x445566, 255 },
        { WidgetPropertyType::COLOR_PRIMARY_INACTIVE, "#55667720", 0x556677, 32 },
        { WidgetPropertyType::COLOR_SECONDARY_ACTIVE, "#667788C0", 0x667788, 192 }
    };
    WidgetPropertyStore store;
    const LvglColor fallback(0xABCDEF, 64);
    auto check = [&](WidgetPropertyType type, LvglColor expected) {
        const auto actual = store.ResolveColor(type, fallback);
        zassert_equal(lv_color_to_u32(actual.ToLvColor()), lv_color_to_u32(expected.ToLvColor()));
        zassert_equal(actual.ToLvOpa(), expected.ToLvOpa());
    };
    for(const auto& color : cases)
        store.Register(color.type, std::pmr::string(color.text), PropertyChangeEffect::Repaint);

    for(const auto& color : cases) {
        check(color.type, LvglColor(color.rgb, color.alpha));
        zassert_true(store.Set(color.type, std::pmr::string("#FEDCBA80")));
    }
    for(const auto& color : cases)
        check(color.type, LvglColor(color.rgb, color.alpha));

    store.ApplyColor(cases[0].type);
    check(cases[0].type, LvglColor(0xFEDCBA, 128));
    for(const auto& color : cases) {
        if(color.type != cases[0].type)
            check(color.type, LvglColor(color.rgb, color.alpha));
    }
    for(const auto& color : cases) {
        store.ApplyColor(color.type);
        check(color.type, LvglColor(0xFEDCBA, 128));
        zassert_true(store.Set(color.type, std::pmr::string{}));
        check(color.type, LvglColor(0xFEDCBA, 128));
        store.ApplyColor(color.type);
        check(color.type, fallback);
    }
    store.ApplyColor(WidgetPropertyType::VALUE);
    check(WidgetPropertyType::VALUE, fallback);
    check(static_cast<WidgetPropertyType>(UINT16_MAX), fallback);
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
        zassert_equal(widget->IsProcessingEligible(), expected > 0);
        for(const auto& value : invalid_values) {
            PublishSensor(SENSOR_ID, value);
            zassert_equal(std::get<int>(widget->Read(WidgetPropertyType::OPACITY)), expected);
            zassert_true(widget->notified.empty());
        }
    }
}

ZTEST(widget_bindings, test_animation_store_validates_before_mutation) {
    WidgetPropertyStore store;
    store.Register(WidgetPropertyType::ANIMATION_TYPE,
        static_cast<int>(Animation::DEFAULT_TYPE), PropertyChangeEffect::None);
    store.Register(WidgetPropertyType::IS_ANIMATION_ACTIVE, Animation::DEFAULT_ACTIVE, PropertyChangeEffect::None);
    store.Register(WidgetPropertyType::ANIMATION_DURATION_MS,
        Animation::DEFAULT_DURATION_MS, PropertyChangeEffect::None);
    zassert_equal(std::get<int>(store.Get(WidgetPropertyType::ANIMATION_TYPE)), 0);
    zassert_false(std::get<bool>(store.Get(WidgetPropertyType::IS_ANIMATION_ACTIVE)));
    zassert_equal(std::get<int>(store.Get(WidgetPropertyType::ANIMATION_DURATION_MS)), 1000);

    const ConfigValue invalid_numbers[] = {
        {}, INT32_MIN, -1, true, false, 0.0, 1.0, 2.0, 2.5, 1000.0,
        static_cast<double>(INT32_MAX) + 1.0, std::pmr::string("2"),
        std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity()
    };
    for(auto type : { WidgetPropertyType::ANIMATION_TYPE, WidgetPropertyType::ANIMATION_DURATION_MS }) {
        const std::vector<int> valid_values = type == WidgetPropertyType::ANIMATION_TYPE
            ? std::vector<int>{ 0, 1, 2 } : std::vector<int>{ 2, 3, 1000, INT32_MAX };
        for(int accepted : valid_values) {
            zassert_true(store.Set(type, accepted));
            auto reject = [&](const ConfigValue& value) {
                zassert_false(store.Set(type, value));
                zassert_equal(std::get<int>(store.Get(type)), accepted);
            };
            for(const auto& value : invalid_numbers)
                reject(value);
            if(type == WidgetPropertyType::ANIMATION_TYPE) {
                reject(3);
                reject(INT32_MAX);
            } else {
                reject(0);
                reject(1);
            }
        }
    }
    const ConfigValue invalid_active[] = {
        {}, 0, 1, -1, 2, 0.0, 1.0, 0.5, std::pmr::string("true"),
        std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()
    };
    for(bool accepted : { true, false }) {
        zassert_true(store.Set(WidgetPropertyType::IS_ANIMATION_ACTIVE, accepted));
        for(const auto& value : invalid_active) {
            zassert_false(store.Set(WidgetPropertyType::IS_ANIMATION_ACTIVE, value));
            zassert_equal(std::get<bool>(store.Get(WidgetPropertyType::IS_ANIMATION_ACTIVE)), accepted);
        }
    }
}

ZTEST(widget_bindings, test_animation_numeric_bindings_reject_floats_overflow_and_invalid_values) {
    const EventData invalid_numbers[] = {
        INT32_MIN, -1, UINT32_MAX, static_cast<uint32_t>(INT32_MAX) + 1U,
        true, false, 0.0F, 1.0F, 2.0F, 2.5F, 1000.0F, static_cast<float>(INT32_MAX), std::string("2"),
        std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()
    };
    for(auto type : { WidgetPropertyType::ANIMATION_TYPE, WidgetPropertyType::ANIMATION_DURATION_MS }) {
        auto configuration = MakeConfiguration();
        configuration->bindings.push_back(SensorBinding(type, SENSOR_ID));
        auto widget = MakeActiveWidget(std::move(configuration));
        const std::vector<int> valid_values = type == WidgetPropertyType::ANIMATION_TYPE
            ? std::vector<int>{ 0, 1, 2 } : std::vector<int>{ 2, 3, 1000, INT32_MAX };
        for(int accepted : valid_values) {
            for(const EventData& input : { EventData{accepted}, EventData{static_cast<uint32_t>(accepted)} }) {
                PublishSensor(SENSOR_ID, input);
                zassert_equal(std::get<int>(widget->Read(type)), accepted);
                widget->notified.clear();
                auto reject = [&](const EventData& value) {
                    PublishSensor(SENSOR_ID, value);
                    zassert_equal(std::get<int>(widget->Read(type)), accepted);
                    zassert_true(widget->notified.empty());
                };
                for(const auto& value : invalid_numbers)
                    reject(value);
                const int invalid_value = type == WidgetPropertyType::ANIMATION_TYPE ? 3 : 0;
                reject(invalid_value);
                reject(static_cast<uint32_t>(invalid_value));
                if(type == WidgetPropertyType::ANIMATION_TYPE) {
                    reject(INT32_MAX);
                    reject(static_cast<uint32_t>(INT32_MAX));
                } else {
                    reject(1);
                    reject(uint32_t{1});
                }
            }
        }
    }
}

ZTEST(widget_bindings, test_animation_active_binding_accepts_only_booleans_or_exact_zero_one) {
    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::IS_ANIMATION_ACTIVE, SENSOR_ID));
    auto widget = MakeActiveWidget(std::move(configuration));
    const EventData invalid_values[] = {
        -1, 2, INT32_MIN, INT32_MAX, uint32_t{2}, UINT32_MAX, -1.0F, 0.5F, 2.0F,
        std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(), std::string("true"), std::string("1")
    };
    for(bool accepted : { true, false }) {
        const EventData valid_values[] = {
            accepted, static_cast<int>(accepted), static_cast<uint32_t>(accepted), static_cast<float>(accepted)
        };
        for(const auto& value : valid_values) {
            PublishSensor(SENSOR_ID, value);
            zassert_equal(std::get<bool>(widget->Read(WidgetPropertyType::IS_ANIMATION_ACTIVE)), accepted);
            widget->notified.clear();
            for(const auto& invalid : invalid_values) {
                PublishSensor(SENSOR_ID, invalid);
                zassert_equal(std::get<bool>(widget->Read(WidgetPropertyType::IS_ANIMATION_ACTIVE)), accepted);
                zassert_true(widget->notified.empty());
            }
        }
    }
}

ZTEST(widget_bindings, test_animation_bindings_follow_ordinary_tracking_policy) {
    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::ANIMATION_TYPE, "type"));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::IS_ANIMATION_ACTIVE, "enabled"));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::ANIMATION_DURATION_MS, "duration"));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::IS_VISIBLE, "visible"));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::IS_ACTIVE, "active"));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::OPACITY, "opacity"));
    auto widget = MakeActiveWidget(configuration);

    auto update = [&](int type, bool enabled, int duration) {
        PublishSensor("type", type);
        PublishSensor("enabled", enabled);
        PublishSensor("duration", duration);
    };
    auto check = [&](int type, bool enabled, int duration) {
        zassert_equal(std::get<int>(widget->Read(WidgetPropertyType::ANIMATION_TYPE)), type);
        zassert_equal(std::get<bool>(widget->Read(WidgetPropertyType::IS_ANIMATION_ACTIVE)), enabled);
        zassert_equal(std::get<int>(widget->Read(WidgetPropertyType::ANIMATION_DURATION_MS)), duration);
    };
    PublishSensor("visible", false);
    update(1, true, 333);
    check(1, true, 333);
    zassert_true(widget->notified.empty());
    PublishSensor("duration", 1.5F);
    check(1, true, 333);
    PublishSensor("visible", true);
    zassert_equal(widget->notified.size(), 3U);
    widget->notified.clear();

    PublishSensor("opacity", 0);
    update(2, false, 222);
    check(2, false, 222);
    zassert_true(widget->notified.empty());
    PublishSensor("opacity", 255);
    zassert_equal(widget->notified.size(), 3U);
    widget->notified.clear();

    widget->OnDeactivated();
    update(1, true, 444);
    check(1, true, 444);
    zassert_true(widget->notified.empty());
    widget->OnActivated();
    widget->notified.clear();

    PublishSensor("active", false);
    update(2, false, 555);
    check(1, true, 444);
    zassert_true(widget->notified.empty());
    PublishSensor("active", true);
    check(1, true, 444);
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
    zassert_equal(widget->notified.size(), 1U);

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

ZTEST(widget_bindings, test_a_hidden_group_tracks_updates_and_replays_on_activation) {
    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));

    auto widget = MakeActiveWidget(std::move(configuration));

    PublishSensor(SENSOR_ID, 7.0F);
    widget->notified.clear();
    widget->OnDeactivated();

    PublishSensor(SENSOR_ID, 42.5F);

    zassert_within(widget->ReadNumber(WidgetPropertyType::VALUE), 42.5, 0.001);
    zassert_true(widget->notified.empty(), "A hidden widget must not render.");

    widget->OnActivated();

    zassert_false(widget->notified.empty(), "Activation must replay the store.");
    zassert_within(widget->ReadNumber(WidgetPropertyType::VALUE), 42.5, 0.001);
    PublishSensor(SENSOR_ID, 55.0F);
    zassert_within(widget->ReadNumber(WidgetPropertyType::VALUE), 55.0, 0.001);
}

ZTEST(widget_bindings, test_only_explicit_inactivity_suspends_incoming_tracking) {
    for(auto target : { WidgetPropertyType::IS_ACTIVE, WidgetPropertyType::IS_VISIBLE, WidgetPropertyType::OPACITY }) {
        auto configuration = MakeConfiguration();
        configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));
        configuration->bindings.push_back(SensorBinding(target, OTHER_SENSOR_ID));
        auto widget = MakeActiveWidget(std::move(configuration));
        zassert_true(widget->IsActive());
        zassert_true(widget->IsProcessingEligible());
        PublishSensor(SENSOR_ID, 7.0F);
        PublishSensor(OTHER_SENSOR_ID, 0);
        widget->notified.clear();
        zassert_false(widget->IsProcessingEligible());
        PublishSensor(SENSOR_ID, 42.5F);
        const double expected = target == WidgetPropertyType::IS_ACTIVE ? 7.0 : 42.5;
        zassert_equal(widget->ReadNumber(WidgetPropertyType::VALUE), expected);
        zassert_true(widget->notified.empty());
        if(target != WidgetPropertyType::IS_VISIBLE)
            zassert_true(widget->IsVisible());
        if(target != WidgetPropertyType::IS_ACTIVE)
            zassert_true(widget->IsActive());
        PublishSensor(OTHER_SENSOR_ID, 1);
        zassert_true(widget->IsProcessingEligible());
        zassert_equal(widget->ReadNumber(WidgetPropertyType::VALUE), expected);
        zassert_equal(widget->notified.size(), target == WidgetPropertyType::IS_ACTIVE ? 0U : 1U);
        PublishSensor(SENSOR_ID, 55.0F);
        zassert_equal(widget->ReadNumber(WidgetPropertyType::VALUE), 55.0);
    }
}

ZTEST(widget_bindings, test_management_updates_work_while_multiple_conditions_suspend_processing) {
    auto configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::IS_ACTIVE] = false;
    configuration->properties[WidgetPropertyType::IS_VISIBLE] = false;
    configuration->properties[WidgetPropertyType::OPACITY] = 0;
    configuration->properties[WidgetPropertyType::VALUE] = 7.0;
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::IS_ACTIVE, "activity"));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::IS_VISIBLE, "visibility"));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::OPACITY, "opacity"));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));
    auto widget = MakeActiveWidget(std::move(configuration));
    zassert_false(widget->IsActive());
    zassert_false(widget->IsProcessingEligible());
    widget->OnDeactivated();
    PublishSensor("activity", true);
    zassert_true(widget->IsActive());
    zassert_false(widget->IsProcessingEligible());
    PublishSensor("visibility", true);
    zassert_true(widget->IsVisible());
    zassert_false(lv_obj_has_flag(widget->GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN));
    zassert_false(widget->IsProcessingEligible());
    PublishSensor("opacity", 128);
    zassert_equal(widget->ReadNumber(WidgetPropertyType::OPACITY), 128);
    zassert_false(widget->IsProcessingEligible());
    PublishSensor(SENSOR_ID, 99.0F);
    widget->OnActivated();
    zassert_true(widget->IsProcessingEligible());
    zassert_equal(widget->ReadNumber(WidgetPropertyType::VALUE), 99.0);
    PublishSensor(SENSOR_ID, 55.0F);
    zassert_equal(widget->ReadNumber(WidgetPropertyType::VALUE), 55.0);
}

ZTEST(widget_bindings, test_invalid_management_flags_cannot_reactivate_a_widget) {
    const EventData invalid_values[] = {
        -1, 2, uint32_t{2}, UINT32_MAX, -0.5F, 1.5F, std::string("true")
    };
    for(auto target : { WidgetPropertyType::IS_ACTIVE, WidgetPropertyType::IS_VISIBLE }) {
        auto configuration = MakeConfiguration();
        configuration->properties[target] = false;
        configuration->bindings.push_back(SensorBinding(target, SENSOR_ID));
        auto widget = MakeActiveWidget(std::move(configuration));
        for(const auto& value : invalid_values) {
            PublishSensor(SENSOR_ID, value);
            zassert_false(std::get<bool>(widget->Read(target)));
            zassert_false(widget->IsProcessingEligible());
        }
        PublishSensor(SENSOR_ID, true);
        zassert_true(widget->IsProcessingEligible());
    }
}

ZTEST(widget_bindings, test_ancestor_visibility_and_opacity_preserve_incoming_tracking) {
    auto root = MakeRoot();
    auto parent = std::make_shared<Frame>(Frame::CreateWrapped(root->GetObject()).Build());
    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::IS_VISIBLE, "visibility"));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::OPACITY, "opacity"));
    auto widget = MakeActiveWidget(std::move(configuration), parent);
    PublishSensor(SENSOR_ID, 7.0F);
    lv_obj_add_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(parent->GetObject(), 0, LV_PART_MAIN);
    PublishSensor("visibility", false);
    PublishSensor("opacity", 0);
    PublishSensor("visibility", true);
    PublishSensor("opacity", 128);
    zassert_true(widget->IsVisible());
    zassert_false(widget->IsProcessingEligible());
    PublishSensor(SENSOR_ID, 42.5F);
    lv_obj_remove_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN);
    zassert_false(widget->IsProcessingEligible());
    PublishSensor(SENSOR_ID, 55.0F);
    lv_obj_set_style_opa(parent->GetObject(), 128, LV_PART_MAIN);
    zassert_true(widget->IsProcessingEligible());
    zassert_equal(widget->ReadNumber(WidgetPropertyType::VALUE), 55.0);
    PublishSensor(SENSOR_ID, 80.0F);
    zassert_equal(widget->ReadNumber(WidgetPropertyType::VALUE), 80.0);
}

ZTEST(widget_bindings, test_transparent_visual_parts_do_not_suspend_tracking) {
    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));
    auto widget = MakeActiveWidget(std::move(configuration));
    auto child = std::make_shared<Frame>(Frame::CreateWrapped(widget->GetContainer()->GetObject()).Build());
    widget->GetContainer()->SetChild(child);
    lv_obj_set_style_opa(child->GetObject(), 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(widget->GetContainer()->GetObject(), 0, LV_PART_MAIN);
    zassert_true(widget->IsProcessingEligible());
    PublishSensor(SENSOR_ID, 42.5F);
    zassert_equal(widget->ReadNumber(WidgetPropertyType::VALUE), 42.5);
}

ZTEST(widget_bindings, test_only_owning_widget_inactivity_suspends_descendant_tracking) {
    auto owner_configuration = MakeConfiguration();
    owner_configuration->bindings.push_back(SensorBinding(WidgetPropertyType::OPACITY, "owner_opacity"));
    owner_configuration->bindings.push_back(SensorBinding(WidgetPropertyType::IS_ACTIVE, "owner_activity"));
    auto owner = MakeActiveWidget(std::move(owner_configuration));
    auto parent = std::make_shared<Frame>(Frame::CreateWrapped(owner->GetContainer()->GetObject())
        .SetProcessingParent(owner->GetContainer()).Build());
    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::OPACITY, "child_opacity"));
    auto widget = MakeActiveWidget(std::move(configuration), parent);
    PublishSensor(SENSOR_ID, 7.0F);
    PublishSensor("owner_opacity", 0);
    zassert_false(widget->IsProcessingEligible());
    PublishSensor("child_opacity", 128);
    PublishSensor(SENSOR_ID, 55.0F);
    PublishSensor("owner_opacity", 128);
    zassert_true(widget->IsProcessingEligible());
    zassert_equal(widget->ReadNumber(WidgetPropertyType::VALUE), 55.0);
    PublishSensor("owner_activity", false);
    PublishSensor(SENSOR_ID, 80.0F);
    zassert_false(widget->IsProcessingEligible());
    PublishSensor("owner_activity", true);
    zassert_true(widget->IsProcessingEligible());
    zassert_equal(widget->ReadNumber(WidgetPropertyType::VALUE), 55.0);
    PublishSensor(SENSOR_ID, 42.0F);
    zassert_equal(widget->ReadNumber(WidgetPropertyType::VALUE), 42.0);
}

ZTEST(widget_bindings, test_hidden_screen_can_resume_without_reactivating_its_group) {
    using eerie_leap::domain::ui_domain::models::ScreenConfiguration;
    using eerie_leap::views::screens::Screen;
    using eerie_leap::views::screens::ScreenGroup;

    class TestScreen : public Screen {
    public:
        using Screen::Screen;
        using Screen::SetVisibility;
    };

    auto root = MakeRoot();
    auto group = std::make_shared<ScreenGroup>(1, root);
    auto screen = std::make_shared<TestScreen>(1, group->GetContainer(), WidgetContext{});
    auto screen_configuration = make_shared_pmr<ScreenConfiguration>(Mrm::GetExtPmr());
    screen_configuration->id = 1;
    screen_configuration->screen_group_id = 1;
    screen_configuration->grid.width = 1;
    screen_configuration->grid.height = 1;
    screen_configuration->is_visible = false;
    auto configuration = MakeConfiguration();
    configuration->type = WidgetType::IndicatorBar;
    configuration->size_grid.width = 1;
    configuration->size_grid.height = 1;
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));
    screen_configuration->AddWidget(configuration);
    screen->Configure(screen_configuration);
    group->AddScreen(screen);
    group->Activate();
    zassert_equal(group->EnsureRendered(), 0);
    zassert_equal(screen->GetWidgets()->size(), 1U);
    auto* widget = dynamic_cast<WidgetBase*>(screen->GetWidgets()->front().get());
    zassert_not_null(widget);
    auto* bar = widget->GetContainer()->GetChild()->GetObject();
    zassert_false(widget->IsProcessingEligible());
    PublishSensor(SENSOR_ID, 42.0F);
    screen->SetVisibility(true);
    lv_refr_now(nullptr);
    zassert_true(widget->IsProcessingEligible());
    zassert_equal(lv_bar_get_value(bar), 42);
    PublishSensor(SENSOR_ID, 7.0F);
    zassert_equal(lv_bar_get_value(bar), 7);
    screen->SetVisibility(false);
    PublishSensor(SENSOR_ID, 55.0F);
    zassert_equal(lv_bar_get_value(bar), 7);
    screen->SetVisibility(true);
    lv_refr_now(nullptr);
    zassert_equal(lv_bar_get_value(bar), 55);
    PublishSensor(SENSOR_ID, 80.0F);
    zassert_equal(lv_bar_get_value(bar), 80);
}

ZTEST(widget_bindings, test_suspended_local_updates_do_not_write_or_publish) {
    for(auto target : { WidgetPropertyType::IS_ACTIVE, WidgetPropertyType::IS_VISIBLE, WidgetPropertyType::OPACITY }) {
        auto configuration = MakeConfiguration();
        configuration->bindings.push_back(SettingBinding(PropertyBindingDirection::InOut));
        configuration->bindings.push_back(SensorBinding(target, SENSOR_ID));
        configuration->properties[WidgetPropertyType::VALUE] = 7.0;
        auto widget = MakeActiveWidget(std::move(configuration));
        ChangeRequestProbe probe;
        PublishSensor(SENSOR_ID, 0);
        widget->WriteLocal(WidgetPropertyType::VALUE, ConfigValue{55.0});
        zassert_equal(widget->ReadNumber(WidgetPropertyType::VALUE), 7.0);
        zassert_true(probe.WaitForNoRequest());
        PublishSensor(SENSOR_ID, 1);
        widget->WriteLocal(WidgetPropertyType::VALUE, ConfigValue{80.0});
        zassert_true(probe.WaitForRequest());
        zassert_equal(widget->ReadNumber(WidgetPropertyType::VALUE), 80.0);
    }
}

ZTEST(widget_bindings, test_hidden_properties_replay_latest_values_once_after_all_visual_gates_open) {
    auto root = MakeRoot();
    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::LABEL, "label"));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::COLOR_PRIMARY_ACTIVE, "color"));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::IS_VISIBLE, "visibility"));
    auto widget = MakeActiveWidget(configuration, root);
    PublishSensor("visibility", false);
    lv_obj_set_style_opa(root->GetObject(), 0, LV_PART_MAIN);
    PublishSensor(SENSOR_ID, 42.0F);
    PublishSensor(SENSOR_ID, 55.0F);
    PublishSensor("label", std::string("Latest label"));
    PublishSensor("color", std::string("#3366FF80"));
    PublishSensor("color", std::string("invalid"));
    zassert_equal(widget->ReadNumber(WidgetPropertyType::VALUE), 55.0);
    zassert_true(std::get<std::pmr::string>(widget->Read(WidgetPropertyType::LABEL)) == "Latest label");
    zassert_true(std::get<std::pmr::string>(widget->Read(WidgetPropertyType::COLOR_PRIMARY_ACTIVE)) == "#3366FF80");
    PublishSensor("visibility", true);
    lv_refr_now(nullptr);
    zassert_true(widget->notified.empty());
    lv_obj_set_style_opa(root->GetObject(), 128, LV_PART_MAIN);
    lv_refr_now(nullptr);
    zassert_equal(widget->notified.size(), 3U);
    widget->notified.clear();
    lv_refr_now(nullptr);
    zassert_true(widget->notified.empty());
}

ZTEST(widget_bindings, test_hidden_slider_replays_range_and_value_without_publishing) {
    using eerie_leap::views::widgets::controls::SliderControl;
    auto root = MakeRoot();
    auto configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::MIN_VALUE] = 0.0;
    configuration->properties[WidgetPropertyType::MAX_VALUE] = 100.0;
    configuration->properties[WidgetPropertyType::STEP] = 1.0;
    configuration->properties[WidgetPropertyType::VALUE] = 7.0;
    configuration->bindings.push_back(SettingBinding(PropertyBindingDirection::InOut));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::MIN_VALUE, "minimum"));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::MAX_VALUE, "maximum"));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::STEP, "step"));
    SliderControl slider(1, root, WidgetContext{});
    slider.Configure(configuration);
    zassert_equal(slider.Render(), 0);
    slider.OnActivated();
    ChangeRequestProbe probe;
    auto* slider_object = slider.GetContainer()->GetChild()->GetObject();
    lv_obj_add_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN);
    PublishSettingChanged(55.0F);
    PublishSensor("minimum", 50.0F);
    PublishSensor("maximum", 70.0F);
    PublishSensor("step", 5.0F);
    lv_refr_now(nullptr);
    zassert_equal(lv_slider_get_value(slider_object), 7);
    lv_obj_remove_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(nullptr);
    zassert_equal(lv_slider_get_max_value(slider_object), 4);
    zassert_equal(lv_slider_get_value(slider_object), 1);
    zassert_true(probe.WaitForNoRequest());

    lv_obj_add_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN);
    PublishSettingChanged(80.0F);
    PublishSensor("maximum", 90.0F);
    PublishSensor("step", 10.0F);
    lv_obj_remove_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN);
    uint32_t key = LV_KEY_RIGHT;
    lv_obj_send_event(slider_object, LV_EVENT_KEY, &key);
    zassert_true(probe.WaitForRequest());
    zassert_equal(lv_slider_get_value(slider_object), 4);
    zassert_within(*probe.Value(), 90.0F, 0.001F);
}

ZTEST(widget_bindings, test_waiting_binding_checks_activity_after_taking_the_lvgl_lock) {
    for(auto target : { WidgetPropertyType::IS_ACTIVE, WidgetPropertyType::IS_VISIBLE }) {
        auto configuration = MakeConfiguration();
        configuration->properties[WidgetPropertyType::VALUE] = 7.0;
        configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));
        configuration->bindings.push_back(SensorBinding(target, OTHER_SENSOR_ID));
        auto widget = MakeActiveWidget(configuration);
        k_thread publisher{};
        k_sem started{};
        k_sem_init(&started, 0, 1);
        {
            eerie_leap::domain::ui_domain::ScopedLvglLock lvgl_guard;
            k_thread_create(&publisher, publisher_stack, K_THREAD_STACK_SIZEOF(publisher_stack),
                [](void* context, void*, void*) {
                    k_sem_give(static_cast<k_sem*>(context));
                    PublishSensor(SENSOR_ID, 55.0F);
                }, &started, nullptr, nullptr, K_PRIO_COOP(0), 0, K_NO_WAIT);
            zassert_equal(k_sem_take(&started, K_MSEC(DISPATCH_TIMEOUT_MS)), 0);
            PublishSensor(OTHER_SENSOR_ID, false);
        }
        zassert_equal(k_thread_join(&publisher, K_MSEC(DISPATCH_TIMEOUT_MS)), 0);
        zassert_equal(widget->ReadNumber(WidgetPropertyType::VALUE), target == WidgetPropertyType::IS_ACTIVE ? 7.0 : 55.0);
        zassert_true(widget->notified.empty());
    }
}

ZTEST(widget_bindings, test_teardown_detaches_waiting_bindings_and_refresh_callbacks) {
    auto* display = lv_display_get_default();
    const auto callback_count = lv_display_get_event_count(display);
    auto configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::VALUE] = 7.0;
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));
    auto widget = MakeActiveWidget(configuration);
    auto store = widget->GetStore();
    k_thread publisher{};
    k_sem started{};
    k_sem_init(&started, 0, 1);
    {
        eerie_leap::domain::ui_domain::ScopedLvglLock lvgl_guard;
        k_thread_create(&publisher, publisher_stack, K_THREAD_STACK_SIZEOF(publisher_stack),
            [](void* context, void*, void*) {
                k_sem_give(static_cast<k_sem*>(context));
                PublishSensor(SENSOR_ID, 55.0F);
            }, &started, nullptr, nullptr, K_PRIO_COOP(0), 0, K_NO_WAIT);
        zassert_equal(k_sem_take(&started, K_MSEC(DISPATCH_TIMEOUT_MS)), 0);
        widget.reset();
    }
    zassert_equal(k_thread_join(&publisher, K_MSEC(DISPATCH_TIMEOUT_MS)), 0);
    zassert_equal(store->GetAs<double>(WidgetPropertyType::VALUE, -1.0), 7.0);
    zassert_equal(lv_display_get_event_count(display), callback_count);
    lv_refr_now(nullptr);
}

ZTEST(widget_bindings, test_suspended_slider_and_toggle_reject_native_input) {
    using eerie_leap::views::widgets::controls::SliderControl;
    using eerie_leap::views::widgets::controls::ToggleControl;

    auto configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::MIN_VALUE] = 0.0;
    configuration->properties[WidgetPropertyType::MAX_VALUE] = 100.0;
    configuration->properties[WidgetPropertyType::STEP] = 1.0;
    configuration->properties[WidgetPropertyType::VALUE] = 7.0;
    configuration->properties[WidgetPropertyType::OPACITY] = 0;
    SliderControl slider(1, MakeRoot(), WidgetContext{});
    slider.Configure(configuration);
    zassert_equal(slider.Render(), 0);
    slider.OnActivated();
    auto* slider_object = slider.GetContainer()->GetChild()->GetObject();
    uint32_t key = LV_KEY_RIGHT;
    lv_obj_send_event(slider_object, LV_EVENT_KEY, &key);
    zassert_equal(lv_slider_get_value(slider_object), 7);
    lv_slider_set_value(slider_object, 80, LV_ANIM_OFF);
    lv_obj_send_event(slider_object, LV_EVENT_VALUE_CHANGED, nullptr);
    zassert_equal(lv_slider_get_value(slider_object), 7);
    zassert_false(lv_obj_has_state(slider_object, LV_STATE_DISABLED));

    configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::IS_ACTIVE] = false;
    ToggleControl toggle(1, MakeRoot(), WidgetContext{});
    toggle.Configure(configuration);
    zassert_equal(toggle.Render(), 0);
    toggle.OnActivated();
    auto* toggle_object = toggle.GetContainer()->GetChild()->GetObject();
    lv_obj_send_event(toggle_object, LV_EVENT_KEY, &key);
    zassert_false(lv_obj_has_state(toggle_object, LV_STATE_CHECKED));
    lv_obj_add_state(toggle_object, LV_STATE_CHECKED);
    lv_obj_send_event(toggle_object, LV_EVENT_VALUE_CHANGED, nullptr);
    zassert_false(lv_obj_has_state(toggle_object, LV_STATE_CHECKED));
    zassert_false(lv_obj_has_state(toggle_object, LV_STATE_DISABLED));
}

ZTEST(widget_bindings, test_indicator_animation_stops_for_each_suspension_condition) {
    using eerie_leap::views::widgets::indicators::BarIndicator;

    for(auto target : { WidgetPropertyType::IS_ACTIVE, WidgetPropertyType::IS_VISIBLE, WidgetPropertyType::OPACITY }) {
        auto root = MakeRoot();
        auto configuration = MakeConfiguration();
        configuration->properties[WidgetPropertyType::IS_SMOOTHED] = true;
        configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));
        configuration->bindings.push_back(SensorBinding(target, OTHER_SENSOR_ID));
        BarIndicator indicator(1, root, WidgetContext{});
        indicator.Configure(configuration);
        zassert_equal(indicator.Render(), 0);
        indicator.OnActivated();
        PublishSensor(SENSOR_ID, 80.0F);
        zassert_not_null(lv_anim_get(&indicator, nullptr));
        auto* bar = indicator.GetContainer()->GetChild()->GetObject();
        const auto displayed = lv_bar_get_value(bar);
        PublishSensor(OTHER_SENSOR_ID, 0);
        zassert_is_null(lv_anim_get(&indicator, nullptr));
        PublishSensor(SENSOR_ID, 55.0F);
        indicator.OnActivated();
        zassert_is_null(lv_anim_get(&indicator, nullptr));
        zassert_equal(lv_bar_get_value(bar), displayed);
        PublishSensor(OTHER_SENSOR_ID, 1);
        PublishSensor(SENSOR_ID, 42.0F);
        zassert_not_null(lv_anim_get(&indicator, nullptr));
        lv_obj_add_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN);
        lv_tick_inc(100);
        lv_anim_refr_now();
        zassert_is_null(lv_anim_get(&indicator, nullptr));
        zassert_equal(lv_bar_get_value(bar), displayed);
    }
}

ZTEST(widget_bindings, test_initial_value_replays_after_management_restoration) {
    using eerie_leap::views::widgets::indicators::BarIndicator;

    for(auto target : { WidgetPropertyType::IS_ACTIVE, WidgetPropertyType::IS_VISIBLE, WidgetPropertyType::OPACITY }) {
        auto configuration = MakeConfiguration();
        configuration->properties[WidgetPropertyType::VALUE] = 42.0;
        configuration->properties[target] = target == WidgetPropertyType::OPACITY ? ConfigValue{0} : ConfigValue{false};
        configuration->bindings.push_back(SensorBinding(target, OTHER_SENSOR_ID));
        BarIndicator indicator(1, MakeRoot(), WidgetContext{});
        indicator.Configure(configuration);
        zassert_equal(indicator.Render(), 0);
        indicator.OnActivated();
        auto* bar = indicator.GetContainer()->GetChild()->GetObject();
        zassert_equal(lv_bar_get_value(bar), 0);

        // There is no value binding: restoration must apply the configured value by itself.
        PublishSensor(OTHER_SENSOR_ID, 1);
        zassert_equal(lv_bar_get_value(bar), 42);
        lv_refr_now(nullptr);
        zassert_equal(lv_bar_get_value(bar), 42);
    }
}

ZTEST(widget_bindings, test_initial_value_replays_after_ancestor_restoration) {
    using eerie_leap::views::widgets::indicators::BarIndicator;

    for(bool hidden : { false, true }) {
        auto root = MakeRoot();
        if(hidden)
            lv_obj_add_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_set_style_opa(root->GetObject(), 0, LV_PART_MAIN);
        auto configuration = MakeConfiguration();
        configuration->properties[WidgetPropertyType::VALUE] = 42.0;
        BarIndicator indicator(1, root, WidgetContext{});
        indicator.Configure(configuration);
        zassert_equal(indicator.Render(), 0);
        indicator.OnActivated();
        auto* bar = indicator.GetContainer()->GetChild()->GetObject();
        zassert_equal(lv_bar_get_value(bar), 0);

        lv_obj_remove_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(root->GetObject(), 255, LV_PART_MAIN);
        lv_refr_now(nullptr);
        zassert_equal(lv_bar_get_value(bar), 42);
    }
}

ZTEST(widget_bindings, test_indicator_animation_resumes_after_management_restoration) {
    using eerie_leap::views::widgets::indicators::BarIndicator;

    for(auto target : { WidgetPropertyType::IS_ACTIVE, WidgetPropertyType::IS_VISIBLE, WidgetPropertyType::OPACITY }) {
        auto configuration = MakeConfiguration();
        configuration->properties[WidgetPropertyType::IS_SMOOTHED] = true;
        configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));
        configuration->bindings.push_back(SensorBinding(target, OTHER_SENSOR_ID));
        BarIndicator indicator(1, MakeRoot(), WidgetContext{});
        indicator.Configure(configuration);
        zassert_equal(indicator.Render(), 0);
        indicator.OnActivated();
        PublishSensor(SENSOR_ID, 80.0F);
        lv_tick_inc(500);
        lv_anim_refr_now();
        auto* bar = indicator.GetContainer()->GetChild()->GetObject();
        const auto displayed = lv_bar_get_value(bar);
        zassert_true(displayed > 0 && displayed < 80);
        PublishSensor(OTHER_SENSOR_ID, 0);
        zassert_is_null(lv_anim_get(&indicator, nullptr));
        lv_tick_inc(5000);
        lv_anim_refr_now();
        zassert_equal(lv_bar_get_value(bar), displayed);

        // No new value arrives during suspension or after restoration.
        PublishSensor(OTHER_SENSOR_ID, 1);
        zassert_not_null(lv_anim_get(&indicator, nullptr));
        lv_tick_inc(5000);
        lv_anim_refr_now();
        zassert_equal(lv_bar_get_value(bar), 80);
        zassert_is_null(lv_anim_get(&indicator, nullptr));
    }
}

ZTEST(widget_bindings, test_indicator_animation_resumes_after_ancestor_restoration) {
    using eerie_leap::views::widgets::indicators::BarIndicator;

    for(bool hidden : { false, true }) {
        auto root = MakeRoot();
        auto configuration = MakeConfiguration();
        configuration->properties[WidgetPropertyType::IS_SMOOTHED] = true;
        configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));
        BarIndicator indicator(1, root, WidgetContext{});
        indicator.Configure(configuration);
        zassert_equal(indicator.Render(), 0);
        indicator.OnActivated();
        PublishSensor(SENSOR_ID, 80.0F);
        lv_tick_inc(500);
        lv_anim_refr_now();
        auto* bar = indicator.GetContainer()->GetChild()->GetObject();
        const auto displayed = lv_bar_get_value(bar);
        zassert_true(displayed > 0 && displayed < 80);
        if(hidden)
            lv_obj_add_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_set_style_opa(root->GetObject(), 0, LV_PART_MAIN);
        lv_tick_inc(5000);
        lv_anim_refr_now();
        zassert_is_null(lv_anim_get(&indicator, nullptr));
        zassert_equal(lv_bar_get_value(bar), displayed);

        lv_obj_remove_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(root->GetObject(), 255, LV_PART_MAIN);
        lv_refr_now(nullptr);
        zassert_not_null(lv_anim_get(&indicator, nullptr));
        lv_tick_inc(5000);
        lv_anim_refr_now();
        zassert_equal(lv_bar_get_value(bar), 80);
    }
}

ZTEST(widget_bindings, test_hidden_digital_value_replays_with_latest_precision) {
    using eerie_leap::views::widgets::indicators::DigitalIndicator;

    for(bool group_hidden : { false, true }) {
        auto configuration = MakeConfiguration();
        configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));
        configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE_PRECISION, "precision"));
        configuration->bindings.push_back(SensorBinding(WidgetPropertyType::IS_VISIBLE, "visibility"));
        DigitalIndicator indicator(1, MakeRoot(), WidgetContext{});
        indicator.Configure(configuration);
        zassert_equal(indicator.Render(), 0);
        indicator.OnActivated();
        if(group_hidden)
            indicator.OnDeactivated();
        else
            PublishSensor("visibility", false);
        PublishSensor("precision", 2);
        PublishSensor(SENSOR_ID, 12.25F);
        auto* label = indicator.GetContainer()->GetChild()->GetObject();
        zassert_str_equal(lv_label_get_text(label), "0");

        if(group_hidden)
            indicator.OnActivated();
        else
            PublishSensor("visibility", true);
        lv_refr_now(nullptr);
        zassert_str_equal(lv_label_get_text(label), "12.25");
    }
}

ZTEST(widget_bindings, test_chart_restoration_replays_only_unapplied_samples) {
    using eerie_leap::views::widgets::indicators::HorizontalChartIndicator;

    auto configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::VALUE] = 7.0;
    configuration->properties[WidgetPropertyType::IS_VISIBLE] = false;
    configuration->properties[WidgetPropertyType::CHART_POINT_COUNT] = 5;
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::VALUE, SENSOR_ID));
    configuration->bindings.push_back(SensorBinding(WidgetPropertyType::IS_VISIBLE, "visibility"));
    HorizontalChartIndicator indicator(1, MakeRoot(), WidgetContext{});
    indicator.Configure(configuration);
    zassert_equal(indicator.Render(), 0);
    indicator.OnActivated();
    auto* chart = indicator.GetContainer()->GetChild()->GetObject();
    auto* series = lv_chart_get_series_next(chart, nullptr);
    zassert_equal(lv_chart_get_x_start_point(chart, series), 0U);
    PublishSensor("visibility", true);
    zassert_equal(lv_chart_get_x_start_point(chart, series), 1U);

    // A visibility cycle without a new value must not append the old sample again.
    PublishSensor("visibility", false);
    PublishSensor("visibility", true);
    lv_refr_now(nullptr);
    zassert_equal(lv_chart_get_x_start_point(chart, series), 1U);
    PublishSensor("visibility", false);
    PublishSensor(SENSOR_ID, 42.0F);
    PublishSensor(SENSOR_ID, 55.0F);
    PublishSensor("visibility", true);
    zassert_equal(lv_chart_get_x_start_point(chart, series), 2U);
    zassert_equal(lv_chart_get_series_y_array(chart, series)[1], 55);
    lv_refr_now(nullptr);
    zassert_equal(lv_chart_get_x_start_point(chart, series), 2U);
}
