#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <lvgl.h>

#include <eerie_memory.hpp>

#include "subsys/event_bus/event_channel.h"
#include "subsys/event_bus/event_channel_id.h"
#include "subsys/threading/scoped_mutex.h"

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/string/string_helpers.h"

#include "domain/settings_domain/event_bus/settings_events_channel.h"
#include "domain/ui_domain/event_bus/navigation_event_channel.h"
#include "domain/ui_domain/models/navigation_intent.h"
#include "domain/ui_domain/models/property_binding.h"
#include "domain/ui_domain/models/widget_configuration.h"
#include "domain/ui_domain/models/widget_property.h"
#include "domain/ui_domain/models/widget_type.h"
#include "domain/ui_domain/services/navigation_service.h"

#include "event_bus/event_channels.h"

#include "views/utilitites/frame.h"
#include "views/widgets/controls/button_control/button_control.h"
#include "views/widgets/widget_context.h"

#include "views_test_support.h"

using namespace eerie_memory;

using eerie_leap::domain::settings_domain::event_bus::SettingsEventsChannel;
using eerie_leap::domain::settings_domain::event_bus::SettingsEventType;
using eerie_leap::domain::settings_domain::event_bus::SettingsPayloadType;
using eerie_leap::domain::ui_domain::event_bus::NavigationEventChannel;
using eerie_leap::domain::ui_domain::event_bus::NavigationEventType;
using eerie_leap::domain::ui_domain::event_bus::NavigationPayloadType;
using eerie_leap::domain::ui_domain::models::NavigationAction;
using eerie_leap::domain::ui_domain::models::NavigationIntent;
using eerie_leap::domain::ui_domain::models::PropertyBinding;
using eerie_leap::domain::ui_domain::models::PropertyBindingDirection;
using eerie_leap::domain::ui_domain::models::WidgetConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetPropertyType;
using eerie_leap::domain::ui_domain::models::WidgetType;
using eerie_leap::domain::ui_domain::services::NavigationService;
using eerie_leap::event_bus::InitializeEventChannels;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::utilities::string::StringHelpers;
using eerie_leap::utilities::type::ConfigValue;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::widgets::WidgetContext;
using eerie_leap::views::widgets::controls::ButtonControl;
using views_test::CleanTestDisplay;
using views_test::EnsureTestDisplay;

namespace {

constexpr int DISPATCH_TIMEOUT_MS = 1000;
constexpr int NO_DISPATCH_TIMEOUT_MS = 200;
constexpr const char* SETTING_ID = "display.brightness";
constexpr const char* OTHER_SETTING_ID = "display.contrast";
constexpr int GO_TO_GROUP = static_cast<int>(NavigationIntent::GoToGroup);

class NavigationProbe {
public:
    NavigationProbe() {
        k_sem_init(&delivered_, 0, K_SEM_MAX_LIMIT);

        subscription_ = CreateScopedSubscription(
            NavigationEventChannel::GetInstance(), NavigationEventType::Changed,
            [this](const NavigationEventChannel::EventMessage& event) {
                Record(event);
            }
        );
    }

    NavigationProbe(const NavigationProbe&) = delete;
    NavigationProbe& operator=(const NavigationProbe&) = delete;
    ~NavigationProbe() = default;

    bool WaitForEvent(int timeout_ms = DISPATCH_TIMEOUT_MS) {
        return k_sem_take(&delivered_, K_MSEC(timeout_ms)) == 0;
    }

    [[nodiscard]] const std::vector<NavigationAction>& Actions() const {
        return actions_;
    }
    [[nodiscard]] const std::vector<uint32_t>& TargetGroupIds() const {
        return target_group_ids_;
    }
    [[nodiscard]] const std::vector<uint32_t>& TargetScreenIds() const {
        return target_screen_ids_;
    }

private:
    k_sem delivered_{};
    std::vector<NavigationAction> actions_;
    std::vector<uint32_t> target_group_ids_;
    std::vector<uint32_t> target_screen_ids_;

    // Declared last so it unsubscribes before the members a dispatch would touch.
    AnySubscription subscription_;

    void Record(const NavigationEventChannel::EventMessage& event) {
        if(const auto* it = event.payload.find(NavigationPayloadType::Action); it != event.payload.end())
            if(const auto* action = std::get_if<uint32_t>(&it->second))
                actions_.push_back(static_cast<NavigationAction>(*action));

        if(const auto* it = event.payload.find(NavigationPayloadType::TargetGroupId); it != event.payload.end())
            if(const auto* screen_group_id = std::get_if<uint32_t>(&it->second))
                target_group_ids_.push_back(*screen_group_id);

        if(const auto* it = event.payload.find(NavigationPayloadType::TargetScreenId); it != event.payload.end())
            if(const auto* screen_id = std::get_if<uint32_t>(&it->second))
                target_screen_ids_.push_back(*screen_id);

        k_sem_give(&delivered_);
    }
};

std::shared_ptr<Frame> MakeRoot() {
    return std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(200, false).SetHeight(200, false).Build());
}

std::shared_ptr<WidgetConfiguration> MakeConfiguration() {
    auto configuration = make_shared_pmr<WidgetConfiguration>(Mrm::GetDefaultPmr());
    configuration->type = WidgetType::ControlButton;
    configuration->id = 0;
    configuration->position_grid.x = 0;
    configuration->position_grid.y = 0;
    configuration->size_grid.width = 100;
    configuration->size_grid.height = 40;
    configuration->properties[WidgetPropertyType::LABEL] = "Go";

    return configuration;
}

// VALUE on the Settings channel: the button's press goes out as a request for the setting, and the owner's answer,
// the setting as it settled, comes back in.
PropertyBinding ValueBinding(PropertyBindingDirection direction, const char* setting_id = SETTING_ID) {
    return PropertyBinding{ .target = WidgetPropertyType::VALUE,
        .event = { .channel = EventChannelId::Settings,
            .event_type = std::to_underlying(SettingsEventType::Changed),
            .payload_key = std::to_underlying(SettingsPayloadType::Value),
            .selector_key = std::to_underlying(SettingsPayloadType::SettingId),
            .selector_value = std::pmr::string(setting_id, Mrm::GetExtPmr()) },
        .direction = direction,
        .outbound_event_type = std::to_underlying(SettingsEventType::ChangeRequested) };
}

// The owner's answer, published synchronously so a test sees its effect without waiting on the bus worker.
void PublishSettingChanged(const char* setting_id, float value) {
    SettingsEventsChannel::GetInstance().Publish(
        { .source_id = 0,
            .type = SettingsEventType::Changed,
            .payload = { { SettingsPayloadType::SettingId, StringHelpers::GetHash(setting_id) },
                { SettingsPayloadType::Value, value } } }
    );
}

// Watches what a button publishes on its outbound bindings: every request, in order.
class RequestProbe {
public:
    struct Request {
        float value = 0.0F;
        uint32_t setting_id = 0;
    };

    RequestProbe() :
        state_(std::make_shared<State>()) {
        k_sem_init(&state_->delivered, 0, K_SEM_MAX_LIMIT);
        k_mutex_init(&state_->lock);

        subscription_ = CreateScopedSubscription(
            SettingsEventsChannel::GetInstance(), SettingsEventType::ChangeRequested,
            [state = state_](const SettingsEventsChannel::EventMessage& event) {
                Request request;

                if(const auto* it = event.payload.find(SettingsPayloadType::Value); it != event.payload.end())
                    request.value = std::get<float>(it->second);

                if(const auto* it = event.payload.find(SettingsPayloadType::SettingId); it != event.payload.end())
                    request.setting_id = std::get<uint32_t>(it->second);

                {
                    ScopedMutex guard(state->lock);
                    state->requests.push_back(request);
                }

                k_sem_give(&state->delivered);
            }
        );
    }

    RequestProbe(const RequestProbe&) = delete;
    RequestProbe& operator=(const RequestProbe&) = delete;
    ~RequestProbe() = default;

    // Waits for the next `count` requests to arrive.
    bool WaitForRequests(int count = 1) const {
        for(int i = 0; i < count; ++i) {
            if(k_sem_take(&state_->delivered, K_MSEC(DISPATCH_TIMEOUT_MS)) != 0)
                return false;
        }

        return true;
    }
    bool WaitForNoRequest() const {
        return k_sem_take(&state_->delivered, K_MSEC(NO_DISPATCH_TIMEOUT_MS)) != 0;
    }

    [[nodiscard]] std::vector<Request> Requests() const {
        ScopedMutex guard(state_->lock);

        return state_->requests;
    }

private:
    struct State {
        k_sem delivered{};
        k_mutex lock{};
        std::vector<Request> requests;
    };

    std::shared_ptr<State> state_;
    AnySubscription subscription_;
};

// Configure -> render -> activate, the state a button is in while events flow.
std::unique_ptr<ButtonControl>
MakeActiveButton(const std::shared_ptr<WidgetConfiguration>& configuration, WidgetContext context = {}) {
    auto button = std::make_unique<ButtonControl>(0, MakeRoot(), std::move(context));

    button->Configure(configuration);
    zassert_equal(button->Render(), 0, "Expected the button to render.");
    button->OnActivated();

    return button;
}

lv_obj_t* ButtonObject(const ButtonControl& button) {
    return lv_obj_get_child(views_test::WidgetContent(button), 0);
}

// The events LVGL sends for a tap, in its order: the release comes before the click.
void Tap(lv_obj_t* object) {
    lv_obj_send_event(object, LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(object, LV_EVENT_RELEASED, nullptr);
    lv_obj_send_event(object, LV_EVENT_CLICKED, nullptr);
}

void ExpectColors(lv_obj_t* button, uint32_t background, lv_opa_t background_opa, uint32_t text, lv_opa_t text_opa) {
    auto* label = lv_obj_get_child(button, 0);

    zassert_equal(
        lv_color_to_u32(lv_obj_get_style_bg_color(button, LV_PART_MAIN)), lv_color_to_u32(lv_color_hex(background))
    );
    zassert_equal(lv_obj_get_style_bg_opa(button, LV_PART_MAIN), background_opa);
    zassert_equal(
        lv_color_to_u32(lv_obj_get_style_text_color(label, LV_PART_MAIN)), lv_color_to_u32(lv_color_hex(text))
    );
    zassert_equal(lv_obj_get_style_text_opa(label, LV_PART_MAIN), text_opa);
}

// Builds a rendered button bound to `navigation_service` and clicks it.
void Click(
    const std::shared_ptr<NavigationService>& navigation_service,
    const std::shared_ptr<WidgetConfiguration>& configuration
) {

    auto root = MakeRoot();
    ButtonControl button(0, root, WidgetContext{ .navigation_service = navigation_service });

    button.Configure(configuration);
    zassert_equal(button.Render(), 0, "Expected the button to render.");
    button.OnActivated();

    lv_obj_send_event(lv_obj_get_child(views_test::WidgetContent(button), 0), LV_EVENT_CLICKED, nullptr);
}

// Both buses and every channel in the registry: a binding resolves its channel there, and the navigation and
// settings channels are inert until a bus drains them.
void* StartEventBus() {
    InitializeEventChannels();

    return nullptr;
}

// Let anything the previous test published drain while no probe is subscribed.
void SettleEventBus(void* fixture) {
    k_msleep(20);
    CleanTestDisplay(fixture);
}

} // namespace

ZTEST_SUITE(button_control, NULL, StartEventBus, NULL, SettleEventBus, NULL);

// A target alone no longer reads as GoToGroup: the intent says what a click does.
ZTEST(button_control, test_a_button_with_only_a_target_group_does_not_navigate) {
    EnsureTestDisplay();

    auto navigation_service = std::make_shared<NavigationService>();
    navigation_service->SetGroupIds({ 0, 1 });
    navigation_service->SetActiveGroupId(0);

    NavigationProbe probe;

    auto configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::TARGET_SCREEN_GROUP] = 1;

    Click(navigation_service, configuration);

    zassert_false(probe.WaitForEvent(NO_DISPATCH_TIMEOUT_MS), "Expected no navigation change.");
}

ZTEST(button_control, test_a_button_can_show_a_target_screen_as_an_overlay) {
    EnsureTestDisplay();

    auto navigation_service = std::make_shared<NavigationService>();
    navigation_service->SetGroupIds({ 0 });
    navigation_service->SetActiveGroupId(0);

    NavigationProbe probe;

    auto configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::NAVIGATION_INTENT] = static_cast<int>(NavigationIntent::ShowOverlay);
    configuration->properties[WidgetPropertyType::TARGET_SCREEN_GROUP] = 7;

    Click(navigation_service, configuration);

    zassert_true(probe.WaitForEvent(), "Expected the click to publish a navigation change.");
    zassert_equal(probe.Actions().front(), NavigationAction::ShowOverlay);
    zassert_equal(probe.TargetScreenIds().front(), 7);
    zassert_true(navigation_service->IsOverlayActive());
}

ZTEST(button_control, test_a_button_can_close_the_active_overlay) {
    EnsureTestDisplay();

    auto navigation_service = std::make_shared<NavigationService>();
    navigation_service->SetGroupIds({ 0 });
    navigation_service->SetActiveGroupId(0);

    NavigationProbe probe;

    // The probe is subscribed first so this setup event is accounted for rather than
    // arriving later and being mistaken for the button's.
    zassert_equal(navigation_service->ShowOverlay(7), 0);
    zassert_true(probe.WaitForEvent(), "Expected the overlay to open.");

    auto configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::NAVIGATION_INTENT] = static_cast<int>(NavigationIntent::CloseOverlay);

    Click(navigation_service, configuration);

    zassert_true(probe.WaitForEvent(), "Expected the click to publish a navigation change.");
    zassert_equal(probe.Actions().back(), NavigationAction::CloseOverlay);
    zassert_false(navigation_service->IsOverlayActive());
}

// An intent that needs a target but has none must do nothing rather than act on 0.
ZTEST(button_control, test_a_show_overlay_button_without_a_target_does_nothing) {
    EnsureTestDisplay();

    auto navigation_service = std::make_shared<NavigationService>();
    navigation_service->SetGroupIds({ 0 });
    navigation_service->SetActiveGroupId(0);

    NavigationProbe probe;

    auto configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::NAVIGATION_INTENT] = static_cast<int>(NavigationIntent::ShowOverlay);

    Click(navigation_service, configuration);

    zassert_false(probe.WaitForEvent(NO_DISPATCH_TIMEOUT_MS), "Expected no navigation change.");
    zassert_false(navigation_service->IsOverlayActive());
}

ZTEST(button_control, test_a_button_without_any_navigation_property_does_nothing) {
    EnsureTestDisplay();

    auto navigation_service = std::make_shared<NavigationService>();
    navigation_service->SetGroupIds({ 0, 1 });
    navigation_service->SetActiveGroupId(0);

    NavigationProbe probe;

    Click(navigation_service, MakeConfiguration());

    zassert_false(probe.WaitForEvent(NO_DISPATCH_TIMEOUT_MS), "Expected no navigation change.");
}

// Next needs no target, so it must dispatch on the intent alone.
ZTEST(button_control, test_a_button_can_carry_an_intent_that_needs_no_target) {
    EnsureTestDisplay();

    auto navigation_service = std::make_shared<NavigationService>();
    navigation_service->SetGroupIds({ 0, 1 });
    navigation_service->SetActiveGroupId(0);

    NavigationProbe probe;

    auto configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::NAVIGATION_INTENT] = static_cast<int>(NavigationIntent::NextGroup);

    Click(navigation_service, configuration);

    zassert_true(probe.WaitForEvent(), "Expected the click to publish a navigation change.");
    zassert_equal(probe.Actions().front(), NavigationAction::ShowGroup);
    zassert_equal(probe.TargetGroupIds().front(), 1);
}

ZTEST(button_control, test_suspended_buttons_do_not_navigate_or_use_disabled_colors) {
    EnsureTestDisplay();
    auto navigation_service = std::make_shared<NavigationService>();
    navigation_service->SetGroupIds({ 0, 1 });
    navigation_service->SetActiveGroupId(0);
    NavigationProbe probe;

    for(auto target : { WidgetPropertyType::IS_ACTIVE, WidgetPropertyType::IS_VISIBLE, WidgetPropertyType::OPACITY }) {
        auto root = MakeRoot();
        auto configuration = MakeConfiguration();
        configuration->properties[WidgetPropertyType::NAVIGATION_INTENT] = GO_TO_GROUP;
        configuration->properties[WidgetPropertyType::TARGET_SCREEN_GROUP] = 1;
        configuration->properties[target] = target == WidgetPropertyType::OPACITY ? ConfigValue{ 0 }
                                                                                  : ConfigValue{ false };
        ButtonControl button(0, root, WidgetContext{ .navigation_service = navigation_service });
        button.Configure(configuration);
        zassert_equal(button.Render(), 0);
        button.OnActivated();
        lv_obj_send_event(lv_obj_get_child(views_test::WidgetContent(button), 0), LV_EVENT_CLICKED, nullptr);
        zassert_false(lv_obj_has_state(lv_obj_get_child(views_test::WidgetContent(button), 0), LV_STATE_DISABLED));
    }
    zassert_false(probe.WaitForEvent(NO_DISPATCH_TIMEOUT_MS));

    auto root = MakeRoot();
    auto configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::NAVIGATION_INTENT] = GO_TO_GROUP;
    configuration->properties[WidgetPropertyType::TARGET_SCREEN_GROUP] = 1;
    ButtonControl button(0, root, WidgetContext{ .navigation_service = navigation_service });
    button.Configure(configuration);
    zassert_equal(button.Render(), 0);
    button.OnActivated();
    button.OnDeactivated();
    lv_obj_send_event(lv_obj_get_child(views_test::WidgetContent(button), 0), LV_EVENT_CLICKED, nullptr);
    button.OnActivated();
    lv_obj_set_style_opa(root->GetObject(), 0, LV_PART_MAIN);
    lv_obj_send_event(lv_obj_get_child(views_test::WidgetContent(button), 0), LV_EVENT_CLICKED, nullptr);
    zassert_false(probe.WaitForEvent(NO_DISPATCH_TIMEOUT_MS));
    lv_obj_set_style_opa(root->GetObject(), 255, LV_PART_MAIN);
    lv_obj_send_event(lv_obj_get_child(views_test::WidgetContent(button), 0), LV_EVENT_CLICKED, nullptr);
    zassert_true(probe.WaitForEvent());
}

ZTEST(button_control, test_a_bound_button_publishes_1_when_pressed_and_0_when_released_or_when_the_press_is_lost) {
    EnsureTestDisplay();

    for(const auto end_of_press : { LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST }) {
        auto configuration = MakeConfiguration();
        configuration->bindings.push_back(ValueBinding(PropertyBindingDirection::Out));

        auto button = MakeActiveButton(configuration);
        auto* object = ButtonObject(*button);
        RequestProbe probe;

        lv_obj_send_event(object, LV_EVENT_PRESSED, nullptr);
        zassert_true(probe.WaitForRequests(), "Expected the press.");

        // Each change goes out once: LVGL saying it again, or ending a press that is over, adds nothing.
        lv_obj_send_event(object, LV_EVENT_PRESSED, nullptr);
        zassert_true(probe.WaitForNoRequest(), "Expected the press to go out once.");

        lv_obj_send_event(object, end_of_press, nullptr);
        zassert_true(probe.WaitForRequests(), "Expected the release.");

        lv_obj_send_event(object, LV_EVENT_RELEASED, nullptr);
        lv_obj_send_event(object, LV_EVENT_PRESS_LOST, nullptr);
        zassert_true(probe.WaitForNoRequest(), "Expected the release to go out once.");

        const auto requests = probe.Requests();
        zassert_equal(requests.size(), 2U);
        zassert_within(requests[0].value, 1.0F, 0.001F);
        zassert_within(requests[1].value, 0.0F, 0.001F);
        zassert_equal(requests[0].setting_id, StringHelpers::GetHash(SETTING_ID));
        zassert_equal(requests[1].setting_id, StringHelpers::GetHash(SETTING_ID));
    }
}

ZTEST(button_control, test_a_press_goes_out_on_each_outbound_binding_with_its_selector) {
    EnsureTestDisplay();

    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(ValueBinding(PropertyBindingDirection::Out, SETTING_ID));
    configuration->bindings.push_back(ValueBinding(PropertyBindingDirection::InOut, OTHER_SETTING_ID));

    auto button = MakeActiveButton(configuration);
    auto* object = ButtonObject(*button);
    RequestProbe probe;

    lv_obj_send_event(object, LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(object, LV_EVENT_RELEASED, nullptr);
    zassert_true(probe.WaitForRequests(4), "Expected the press and the release on both bindings.");
    zassert_true(probe.WaitForNoRequest());

    const auto requests = probe.Requests();
    zassert_equal(requests.size(), 4U);
    const float expected_values[] = { 1.0F, 1.0F, 0.0F, 0.0F };
    const uint32_t expected_settings[] = { StringHelpers::GetHash(SETTING_ID), StringHelpers::GetHash(OTHER_SETTING_ID),
        StringHelpers::GetHash(SETTING_ID), StringHelpers::GetHash(OTHER_SETTING_ID) };
    for(size_t i = 0; i < requests.size(); ++i) {
        zassert_within(requests[i].value, expected_values[i], 0.001F);
        zassert_equal(requests[i].setting_id, expected_settings[i]);
    }
}

// The request/fact split: a press that does not take leaves the button as it was, and only the owner's answer
// changes what it shows.
ZTEST(button_control, test_a_press_never_writes_the_value_the_button_shows) {
    EnsureTestDisplay();

    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(ValueBinding(PropertyBindingDirection::InOut));

    auto button = MakeActiveButton(configuration);
    auto* object = ButtonObject(*button);
    RequestProbe probe;

    lv_obj_send_event(object, LV_EVENT_PRESSED, nullptr);
    zassert_true(probe.WaitForRequests(), "Expected the press.");
    zassert_false(lv_obj_has_state(object, LV_STATE_CHECKED), "Expected the press alone not to light the button.");

    PublishSettingChanged(SETTING_ID, 1.0F);
    zassert_true(lv_obj_has_state(object, LV_STATE_CHECKED));

    lv_obj_send_event(object, LV_EVENT_RELEASED, nullptr);
    zassert_true(probe.WaitForRequests(), "Expected the release.");
    zassert_true(lv_obj_has_state(object, LV_STATE_CHECKED), "Expected the release alone not to turn it off.");

    PublishSettingChanged(SETTING_ID, 0.0F);
    zassert_false(lv_obj_has_state(object, LV_STATE_CHECKED));

    zassert_true(probe.WaitForNoRequest(), "Expected an answer from the owner not to publish.");
    zassert_equal(probe.Requests().size(), 2U);
}

ZTEST(button_control, test_a_suspension_ends_a_press_with_its_release) {
    EnsureTestDisplay();

    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(ValueBinding(PropertyBindingDirection::Out));

    auto button = MakeActiveButton(configuration);
    auto* object = ButtonObject(*button);
    RequestProbe probe;

    lv_obj_send_event(object, LV_EVENT_PRESSED, nullptr);
    zassert_true(probe.WaitForRequests(), "Expected the press.");

    button->OnDeactivated();
    zassert_true(probe.WaitForRequests(), "Expected the suspension to release the press.");

    // The end of the press that LVGL still reports is not sent again, nor is a press it starts while suspended.
    lv_obj_send_event(object, LV_EVENT_RELEASED, nullptr);
    lv_obj_send_event(object, LV_EVENT_PRESSED, nullptr);
    lv_obj_send_event(object, LV_EVENT_PRESS_LOST, nullptr);
    zassert_true(probe.WaitForNoRequest());

    const auto requests = probe.Requests();
    zassert_equal(requests.size(), 2U);
    zassert_within(requests[0].value, 1.0F, 0.001F);
    zassert_within(requests[1].value, 0.0F, 0.001F);
}

ZTEST(button_control, test_a_teardown_ends_a_press_with_its_release) {
    EnsureTestDisplay();

    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(ValueBinding(PropertyBindingDirection::Out));
    RequestProbe probe;

    {
        auto button = MakeActiveButton(configuration);

        lv_obj_send_event(ButtonObject(*button), LV_EVENT_PRESSED, nullptr);
        zassert_true(probe.WaitForRequests(), "Expected the press.");
    }

    zassert_true(probe.WaitForRequests(), "Expected the teardown to release the press.");
    zassert_true(probe.WaitForNoRequest());

    const auto requests = probe.Requests();
    zassert_equal(requests.size(), 2U);
    zassert_within(requests[0].value, 1.0F, 0.001F);
    zassert_within(requests[1].value, 0.0F, 0.001F);
}

ZTEST(button_control, test_a_button_that_was_never_pressed_publishes_nothing) {
    EnsureTestDisplay();

    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(ValueBinding(PropertyBindingDirection::InOut));
    RequestProbe probe;

    {
        auto button = MakeActiveButton(configuration);

        button->OnDeactivated();
        button->OnActivated();
        lv_obj_send_event(ButtonObject(*button), LV_EVENT_RELEASED, nullptr);
        lv_obj_send_event(ButtonObject(*button), LV_EVENT_PRESS_LOST, nullptr);
    }

    zassert_true(probe.WaitForNoRequest(), "Expected no request from a button that was never pressed.");
}

ZTEST(button_control, test_a_suspended_buttons_press_publishes_nothing) {
    EnsureTestDisplay();

    RequestProbe probe;

    for(auto target : { WidgetPropertyType::IS_ACTIVE, WidgetPropertyType::IS_VISIBLE, WidgetPropertyType::OPACITY }) {
        auto configuration = MakeConfiguration();
        configuration->bindings.push_back(ValueBinding(PropertyBindingDirection::Out));
        configuration->properties[target] = target == WidgetPropertyType::OPACITY ? ConfigValue{ 0 }
                                                                                  : ConfigValue{ false };

        auto button = MakeActiveButton(configuration);
        auto* object = ButtonObject(*button);

        lv_obj_send_event(object, LV_EVENT_PRESSED, nullptr);
        zassert_false(lv_obj_has_state(object, LV_STATE_PRESSED));
        lv_obj_send_event(object, LV_EVENT_RELEASED, nullptr);
    }

    zassert_true(probe.WaitForNoRequest(), "Expected no request from a suspended button.");
}

ZTEST(button_control, test_a_bound_button_captures_its_gestures_and_an_unbound_one_lets_them_bubble) {
    EnsureTestDisplay();

    struct Case {
        std::optional<PropertyBindingDirection> direction;
        bool captures_gestures;
    };
    const std::array cases = { Case{ std::nullopt, false }, Case{ PropertyBindingDirection::In, false },
        Case{ PropertyBindingDirection::Out, true }, Case{ PropertyBindingDirection::InOut, true } };

    for(const auto& [direction, captures_gestures] : cases) {
        auto configuration = MakeConfiguration();
        if(direction.has_value())
            configuration->bindings.push_back(ValueBinding(*direction));

        auto button = MakeActiveButton(configuration);

        zassert_equal(lv_obj_is_gesture_bubble(button->GetContainer()->GetObject()), !captures_gestures);
    }
}

ZTEST(button_control, test_an_inbound_value_checks_and_unchecks_the_button) {
    EnsureTestDisplay();

    auto configuration = MakeConfiguration();
    configuration->bindings.push_back(ValueBinding(PropertyBindingDirection::InOut));

    auto button = MakeActiveButton(configuration);
    auto* object = ButtonObject(*button);
    auto* label = lv_obj_get_child(object, 0);
    RequestProbe probe;

    zassert_false(lv_obj_has_state(object, LV_STATE_CHECKED));

    // Anything but 0 lights it.
    for(const float lit : { 1.0F, 160.0F, -1.0F }) {
        PublishSettingChanged(SETTING_ID, lit);
        zassert_true(lv_obj_has_state(object, LV_STATE_CHECKED));
        zassert_true(lv_obj_has_state(label, LV_STATE_CHECKED));

        PublishSettingChanged(SETTING_ID, 0.0F);
        zassert_false(lv_obj_has_state(object, LV_STATE_CHECKED));
        zassert_false(lv_obj_has_state(label, LV_STATE_CHECKED));
    }

    PublishSettingChanged(OTHER_SETTING_ID, 1.0F);
    zassert_false(lv_obj_has_state(object, LV_STATE_CHECKED), "Expected another setting's value not to count.");

    // Inbound never publishes: that is the whole echo suppression rule.
    zassert_true(probe.WaitForNoRequest());
}

ZTEST(button_control, test_a_configured_value_lights_the_button_when_it_renders) {
    EnsureTestDisplay();

    for(const double value : { 0.0, 1.0, 160.0 }) {
        auto configuration = MakeConfiguration();
        configuration->properties[WidgetPropertyType::VALUE] = value;

        auto button = MakeActiveButton(configuration);

        zassert_equal(lv_obj_has_state(ButtonObject(*button), LV_STATE_CHECKED), value != 0.0);
        zassert_equal(lv_obj_has_state(lv_obj_get_child(ButtonObject(*button), 0), LV_STATE_CHECKED), value != 0.0);
    }
}

ZTEST(button_control, test_a_checked_button_wears_the_active_colors_of_a_pressed_one) {
    EnsureTestDisplay();

    auto configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::COLOR_PRIMARY_ACTIVE] = std::pmr::string("#33CC99FF");
    configuration->properties[WidgetPropertyType::COLOR_PRIMARY_INACTIVE] = std::pmr::string("#6688AA80");
    configuration->properties[WidgetPropertyType::COLOR_SECONDARY_ACTIVE] = std::pmr::string("#FFEEDDC0");
    configuration->properties[WidgetPropertyType::COLOR_SECONDARY_INACTIVE] = std::pmr::string("#DDEEFF40");
    configuration->bindings.push_back(ValueBinding(PropertyBindingDirection::In));

    auto button = MakeActiveButton(configuration);
    auto* object = ButtonObject(*button);

    ExpectColors(object, 0x6688AA, 128, 0xDDEEFF, 64);

    PublishSettingChanged(SETTING_ID, 1.0F);
    ExpectColors(object, 0x33CC99, 255, 0xFFEEDD, 192);

    // A press on a checked button keeps the pair, as the pressed state shares it.
    lv_obj_send_event(object, LV_EVENT_PRESSED, nullptr);
    ExpectColors(object, 0x33CC99, 255, 0xFFEEDD, 192);
    lv_obj_send_event(object, LV_EVENT_RELEASED, nullptr);
    ExpectColors(object, 0x33CC99, 255, 0xFFEEDD, 192);

    PublishSettingChanged(SETTING_ID, 0.0F);
    ExpectColors(object, 0x6688AA, 128, 0xDDEEFF, 64);
}

ZTEST(button_control, test_a_click_navigates_whether_the_button_is_bound_or_not) {
    EnsureTestDisplay();

    for(const bool is_bound : { false, true }) {
        auto navigation_service = std::make_shared<NavigationService>();
        navigation_service->SetGroupIds({ 0, 1 });
        navigation_service->SetActiveGroupId(0);

        NavigationProbe navigation_probe;
        RequestProbe request_probe;

        auto configuration = MakeConfiguration();
        configuration->properties[WidgetPropertyType::NAVIGATION_INTENT] = GO_TO_GROUP;
        configuration->properties[WidgetPropertyType::TARGET_SCREEN_GROUP] = 1;
        if(is_bound)
            configuration->bindings.push_back(ValueBinding(PropertyBindingDirection::Out));

        auto button = MakeActiveButton(configuration, WidgetContext{ .navigation_service = navigation_service });
        Tap(ButtonObject(*button));

        zassert_true(navigation_probe.WaitForEvent(), "Expected the click to publish a navigation change.");
        zassert_equal(navigation_probe.Actions().size(), 1);
        zassert_equal(navigation_probe.Actions().front(), NavigationAction::ShowGroup);
        zassert_equal(navigation_probe.TargetGroupIds().front(), 1);

        // The tap that navigated is also a press and its release, for a button that sends them.
        if(is_bound)
            zassert_true(request_probe.WaitForRequests(2), "Expected the press and the release.");
        else
            zassert_true(request_probe.WaitForNoRequest(), "Expected an unbound button to publish nothing.");
    }
}
