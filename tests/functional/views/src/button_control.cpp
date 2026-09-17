#include <cstdint>
#include <memory>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <lvgl.h>

#include <eerie_memory.hpp>

#include "subsys/event_bus/event_bus.h"
#include "subsys/event_bus/event_channel.h"

#include "utilities/memory/memory_resource_manager.h"

#include "domain/ui_domain/event_bus/navigation_event_channel.h"
#include "domain/ui_domain/models/navigation_intent.h"
#include "domain/ui_domain/models/widget_configuration.h"
#include "domain/ui_domain/models/widget_property.h"
#include "domain/ui_domain/models/widget_type.h"
#include "domain/ui_domain/services/navigation_service.h"

#include "views/utilitites/frame.h"
#include "views/widgets/controls/button_control/button_control.h"
#include "views/widgets/widget_context.h"

#include "views_test_support.h"

using namespace eerie_memory;

using eerie_leap::domain::ui_domain::event_bus::NavigationEventChannel;
using eerie_leap::domain::ui_domain::event_bus::NavigationEventType;
using eerie_leap::domain::ui_domain::event_bus::NavigationPayloadType;
using eerie_leap::domain::ui_domain::models::NavigationAction;
using eerie_leap::domain::ui_domain::models::NavigationIntent;
using eerie_leap::domain::ui_domain::models::WidgetConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetPropertyType;
using eerie_leap::domain::ui_domain::models::WidgetType;
using eerie_leap::domain::ui_domain::services::NavigationService;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::event_bus::EventBus;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::widgets::WidgetContext;
using eerie_leap::views::widgets::controls::ButtonControl;
using views_test::CleanTestDisplay;
using views_test::EnsureTestDisplay;

namespace {

constexpr int DISPATCH_TIMEOUT_MS = 1000;
constexpr int NO_DISPATCH_TIMEOUT_MS = 200;
constexpr int BUS_STACK_SIZE = 4096;

// NavigationEventChannel is inert until a bus drains it.
class ButtonBus : public EventBus {
public:
    ButtonBus() : EventBus("button_test_bus", BUS_STACK_SIZE) {
        RegisterChannel(NavigationEventChannel::GetInstance());
    }
};

class NavigationProbe {
public:
    NavigationProbe() {
        k_sem_init(&delivered_, 0, K_SEM_MAX_LIMIT);

        subscription_ = CreateScopedSubscription(
            NavigationEventChannel::GetInstance(),
            NavigationEventType::Changed,
            [this](const NavigationEventChannel::EventMessage& event) { Record(event); });
    }

    NavigationProbe(const NavigationProbe&) = delete;
    NavigationProbe& operator=(const NavigationProbe&) = delete;

    bool WaitForEvent(int timeout_ms = DISPATCH_TIMEOUT_MS) {
        return k_sem_take(&delivered_, K_MSEC(timeout_ms)) == 0;
    }

    [[nodiscard]] const std::vector<NavigationAction>& Actions() const { return actions_; }
    [[nodiscard]] const std::vector<uint32_t>& TargetGroupIds() const { return target_group_ids_; }
    [[nodiscard]] const std::vector<uint32_t>& TargetScreenIds() const { return target_screen_ids_; }

private:
    k_sem delivered_{};
    std::vector<NavigationAction> actions_;
    std::vector<uint32_t> target_group_ids_;
    std::vector<uint32_t> target_screen_ids_;

    // Declared last so it unsubscribes before the members a dispatch would touch.
    AnySubscription subscription_;

    void Record(const NavigationEventChannel::EventMessage& event) {
        if(auto it = event.payload.find(NavigationPayloadType::Action); it != event.payload.end())
            if(const auto* action = std::get_if<uint32_t>(&it->second))
                actions_.push_back(static_cast<NavigationAction>(*action));

        if(auto it = event.payload.find(NavigationPayloadType::TargetGroupId); it != event.payload.end())
            if(const auto* screen_group_id = std::get_if<uint32_t>(&it->second))
                target_group_ids_.push_back(*screen_group_id);

        if(auto it = event.payload.find(NavigationPayloadType::TargetScreenId); it != event.payload.end())
            if(const auto* screen_id = std::get_if<uint32_t>(&it->second))
                target_screen_ids_.push_back(*screen_id);

        k_sem_give(&delivered_);
    }
};

std::shared_ptr<Frame> MakeRoot() {
    return std::make_shared<Frame>(Frame::CreateWrapped()
        .SetWidth(200, false)
        .SetHeight(200, false)
        .Build());
}

// A button renders its lv_button into the container the widget creates under the
// parent, so the clickable object is the first grandchild of the root.
lv_obj_t* FindButton(const std::shared_ptr<Frame>& root) {
    lv_obj_t* container = lv_obj_get_child(root->GetObject(), 0);
    zassert_not_null(container, "Expected the widget to create a container.");

    lv_obj_t* button = lv_obj_get_child(container, 0);
    zassert_not_null(button, "Expected the widget to create a button.");

    return button;
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

// Builds a rendered button bound to `navigation_service` and clicks it.
void Click(
    const std::shared_ptr<NavigationService>& navigation_service,
    const std::shared_ptr<WidgetConfiguration>& configuration) {

    auto root = MakeRoot();
    ButtonControl button(0, root, WidgetContext { .navigation_service = navigation_service });

    button.Configure(configuration);
    zassert_equal(button.Render(), 0, "Expected the button to render.");
    button.OnActivated();

    lv_obj_send_event(FindButton(root), LV_EVENT_CLICKED, nullptr);
}

void* StartEventBus() {
    static ButtonBus bus;

    return nullptr;
}

// Let anything the previous test published drain while no probe is subscribed.
void SettleEventBus(void* fixture) {
    k_msleep(20);
    CleanTestDisplay(fixture);
}

} // namespace

ZTEST_SUITE(button_control, NULL, StartEventBus, NULL, SettleEventBus, NULL);

ZTEST(button_control, test_a_button_with_only_a_target_group_still_switches_groups) {
    EnsureTestDisplay();

    auto navigation_service = std::make_shared<NavigationService>();
    navigation_service->SetGroupIds({ 0, 1 });
    navigation_service->SetActiveGroupId(0);

    NavigationProbe probe;

    auto configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::TARGET_SCREEN_GROUP] = 1;

    Click(navigation_service, configuration);

    zassert_true(probe.WaitForEvent(), "Expected the click to publish a navigation change.");
    zassert_equal(probe.Actions().size(), 1);
    zassert_equal(probe.Actions().front(), NavigationAction::ShowGroup);
    zassert_equal(probe.TargetGroupIds().front(), 1);
}

ZTEST(button_control, test_a_button_can_show_a_target_screen_as_an_overlay) {
    EnsureTestDisplay();

    auto navigation_service = std::make_shared<NavigationService>();
    navigation_service->SetGroupIds({ 0 });
    navigation_service->SetActiveGroupId(0);

    NavigationProbe probe;

    auto configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::NAVIGATION_INTENT] =
        static_cast<int>(NavigationIntent::ShowOverlay);
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
    configuration->properties[WidgetPropertyType::NAVIGATION_INTENT] =
        static_cast<int>(NavigationIntent::CloseOverlay);

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
    configuration->properties[WidgetPropertyType::NAVIGATION_INTENT] =
        static_cast<int>(NavigationIntent::ShowOverlay);

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
    configuration->properties[WidgetPropertyType::NAVIGATION_INTENT] =
        static_cast<int>(NavigationIntent::NextGroup);

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
        configuration->properties[WidgetPropertyType::TARGET_SCREEN_GROUP] = 1;
        configuration->properties[target] = target == WidgetPropertyType::OPACITY
            ? eerie_leap::utilities::type::ConfigValue{0} : eerie_leap::utilities::type::ConfigValue{false};
        ButtonControl button(0, root, WidgetContext { .navigation_service = navigation_service });
        button.Configure(configuration);
        zassert_equal(button.Render(), 0);
        button.OnActivated();
        lv_obj_send_event(FindButton(root), LV_EVENT_CLICKED, nullptr);
        zassert_false(lv_obj_has_state(FindButton(root), LV_STATE_DISABLED));
    }
    zassert_false(probe.WaitForEvent(NO_DISPATCH_TIMEOUT_MS));

    auto root = MakeRoot();
    auto configuration = MakeConfiguration();
    configuration->properties[WidgetPropertyType::TARGET_SCREEN_GROUP] = 1;
    ButtonControl button(0, root, WidgetContext { .navigation_service = navigation_service });
    button.Configure(configuration);
    zassert_equal(button.Render(), 0);
    button.OnActivated();
    button.OnDeactivated();
    lv_obj_send_event(FindButton(root), LV_EVENT_CLICKED, nullptr);
    button.OnActivated();
    lv_obj_set_style_opa(root->GetObject(), 0, LV_PART_MAIN);
    lv_obj_send_event(FindButton(root), LV_EVENT_CLICKED, nullptr);
    zassert_false(probe.WaitForEvent(NO_DISPATCH_TIMEOUT_MS));
    lv_obj_set_style_opa(root->GetObject(), 255, LV_PART_MAIN);
    lv_obj_send_event(FindButton(root), LV_EVENT_CLICKED, nullptr);
    zassert_true(probe.WaitForEvent());
}
