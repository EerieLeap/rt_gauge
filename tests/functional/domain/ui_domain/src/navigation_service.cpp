#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <variant>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/event_bus/event_bus.h"
#include "subsys/event_bus/event_channel.h"

#include "domain/ui_domain/event_bus/navigation_event_channel.h"
#include "domain/ui_domain/models/navigation_intent.h"
#include "domain/ui_domain/services/navigation_service.h"

using eerie_leap::domain::ui_domain::event_bus::NavigationEventChannel;
using eerie_leap::domain::ui_domain::event_bus::NavigationEventType;
using eerie_leap::domain::ui_domain::event_bus::NavigationPayloadType;
using eerie_leap::domain::ui_domain::models::NavigationAction;
using eerie_leap::domain::ui_domain::models::NavigationIntent;
using eerie_leap::domain::ui_domain::services::NavigationService;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::event_bus::EventBus;

namespace {

constexpr int DISPATCH_TIMEOUT_MS = 1000;
constexpr int BUS_STACK_SIZE = 4096;

// NavigationEventChannel is inert until a bus drains it, so the suite owns one.
class TestEventBus : public EventBus {
public:
    TestEventBus() :
        EventBus("nav_test_bus", BUS_STACK_SIZE) {
        RegisterChannel(NavigationEventChannel::GetInstance());
    }
};

// NavigationService never applies a change itself - it publishes an intent and the
// view calls SetActiveGroupId back. These helpers stand in for that view.
void Configure(NavigationService& service, std::vector<uint32_t> screen_group_ids, uint32_t active_screen_group_id) {
    service.SetGroupIds(std::move(screen_group_ids));
    service.SetActiveGroupId(active_screen_group_id);
}

uint32_t ActiveGroupId(const NavigationService& service) {
    auto active_screen_group_id = service.GetActiveGroupId();
    zassert_true(active_screen_group_id.has_value(), "Expected an active screen group.");

    return *active_screen_group_id;
}

// The channel is a singleton, so a probe subscribes for the lifetime of one test only.
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

    [[nodiscard]] bool IsSubscribed() const {
        return subscription_ != nullptr;
    }
    bool WaitForEvent() {
        return k_sem_take(&delivered_, K_MSEC(DISPATCH_TIMEOUT_MS)) == 0;
    }

    [[nodiscard]] size_t Count() const {
        return actions_.size();
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

void* StartEventBus() {
    static TestEventBus bus;

    return nullptr;
}

// Let anything the previous test published drain while no probe is subscribed.
void SettleEventBus(void*) {
    k_msleep(20);
}

} // namespace

ZTEST_SUITE(navigation_service, NULL, StartEventBus, SettleEventBus, NULL, NULL);

ZTEST(navigation_service, test_Next_advances_to_the_following_group) {
    NavigationService service;
    Configure(service, { 0, 1, 2 }, 0);

    zassert_equal(service.Next(), 0);
    zassert_equal(ActiveGroupId(service), 1U);
}

ZTEST(navigation_service, test_Next_wraps_around_to_the_first_group) {
    NavigationService service;
    Configure(service, { 0, 1, 2 }, 2);

    zassert_equal(service.Next(), 0);
    zassert_equal(ActiveGroupId(service), 0U);
}

ZTEST(navigation_service, test_Previous_steps_back_to_the_preceding_group) {
    NavigationService service;
    Configure(service, { 0, 1, 2 }, 2);

    zassert_equal(service.Previous(), 0);
    zassert_equal(ActiveGroupId(service), 1U);
}

ZTEST(navigation_service, test_Previous_wraps_around_to_the_last_group) {
    NavigationService service;
    Configure(service, { 0, 1, 2 }, 0);

    zassert_equal(service.Previous(), 0);
    zassert_equal(ActiveGroupId(service), 2U);
}

ZTEST(navigation_service, test_the_groups_are_ordered_by_id_not_by_insertion) {
    NavigationService service;
    Configure(service, { 7, 2, 5 }, 2);

    zassert_equal(service.Next(), 0);
    zassert_equal(ActiveGroupId(service), 5U);
}

// Regression: the active group has to advance when the request is accepted, not when
// the view reports back, or a burst of swipes all resolves against the same index.
ZTEST(navigation_service, test_consecutive_requests_advance_past_the_pending_one) {
    NavigationService service;
    Configure(service, { 0, 1, 2 }, 0);

    zassert_equal(service.Next(), 0);
    zassert_equal(service.Next(), 0);

    zassert_equal(ActiveGroupId(service), 2U);
}

ZTEST(navigation_service, test_Next_without_an_active_group_reports_ENOENT) {
    NavigationService service;
    service.SetGroupIds({ 0, 1, 2 });

    zassert_equal(service.Next(), -ENOENT);
    zassert_false(service.GetActiveGroupId().has_value());
}

ZTEST(navigation_service, test_Next_without_any_configured_group_reports_ENOENT) {
    NavigationService service;

    zassert_equal(service.Next(), -ENOENT);
    zassert_equal(service.Previous(), -ENOENT);
}

ZTEST(navigation_service, test_Next_over_a_single_group_stays_put) {
    NavigationService service;
    Configure(service, { 4 }, 4);

    zassert_equal(service.Next(), 0);
    zassert_equal(ActiveGroupId(service), 4U);
}

ZTEST(navigation_service, test_GoToGroup_selects_the_requested_group) {
    NavigationService service;
    Configure(service, { 0, 1, 2 }, 0);

    zassert_equal(service.GoToGroup(2), 0);
    zassert_equal(ActiveGroupId(service), 2U);
}

ZTEST(navigation_service, test_GoToGroup_reports_ENOENT_for_an_unknown_group) {
    NavigationService service;
    Configure(service, { 0, 1, 2 }, 1);

    zassert_equal(service.GoToGroup(9), -ENOENT);
    zassert_equal(ActiveGroupId(service), 1U);
}

ZTEST(navigation_service, test_GoToGroup_of_the_active_group_is_a_no_op) {
    NavigationService service;
    NavigationProbe probe;
    zassert_true(probe.IsSubscribed());

    Configure(service, { 0, 1, 2 }, 1);

    zassert_equal(service.GoToGroup(1), 0);
    zassert_false(probe.WaitForEvent(), "No navigation event is expected.");
    zassert_equal(probe.Count(), 0U);
}

ZTEST(navigation_service, test_Back_returns_to_the_previous_group) {
    NavigationService service;
    Configure(service, { 0, 1, 2 }, 0);

    zassert_equal(service.GoToGroup(2), 0);
    zassert_equal(service.Back(), 0);
    zassert_equal(ActiveGroupId(service), 0U);
}

ZTEST(navigation_service, test_Back_without_history_reports_ENOENT) {
    NavigationService service;
    Configure(service, { 0, 1, 2 }, 0);

    zassert_equal(service.Back(), -ENOENT);
    zassert_equal(ActiveGroupId(service), 0U);
}

ZTEST(navigation_service, test_Back_unwinds_the_history_in_reverse_order) {
    NavigationService service;
    Configure(service, { 0, 1, 2 }, 0);

    zassert_equal(service.Next(), 0);
    zassert_equal(service.Next(), 0);
    zassert_equal(ActiveGroupId(service), 2U);

    zassert_equal(service.Back(), 0);
    zassert_equal(ActiveGroupId(service), 1U);

    zassert_equal(service.Back(), 0);
    zassert_equal(ActiveGroupId(service), 0U);

    zassert_equal(service.Back(), -ENOENT);
}

ZTEST(navigation_service, test_Back_does_not_record_history_of_its_own) {
    NavigationService service;
    Configure(service, { 0, 1 }, 0);

    zassert_equal(service.GoToGroup(1), 0);
    zassert_equal(service.Back(), 0);
    zassert_equal(ActiveGroupId(service), 0U);

    // Going back must not have pushed group 1, otherwise Back() would ping-pong forever.
    zassert_equal(service.Back(), -ENOENT);
}

// Regression: the entry may only be consumed once the request has been accepted.
ZTEST(navigation_service, test_Back_keeps_the_history_when_the_request_is_rejected) {
    NavigationService service;
    Configure(service, { 0, 1 }, 0);

    zassert_equal(service.GoToGroup(1), 0);
    zassert_equal(service.ShowOverlay(7), 0);

    zassert_equal(service.Back(), -EBUSY);

    zassert_equal(service.CloseOverlay(), 0);
    zassert_equal(service.Back(), 0);
    zassert_equal(ActiveGroupId(service), 0U);
}

ZTEST(navigation_service, test_the_history_drops_the_oldest_entry_when_it_is_full) {
    NavigationService service;
    Configure(service, { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 }, 0);

    // Nine hops record nine entries into an eight deep history.
    for(uint32_t screen_group_id = 1; screen_group_id <= 9; ++screen_group_id)
        zassert_equal(service.GoToGroup(screen_group_id), 0);

    for(uint32_t expected = 8; expected >= 1; --expected) {
        zassert_equal(service.Back(), 0);
        zassert_equal(ActiveGroupId(service), expected);
    }

    // Group 0 fell off the front of the history.
    zassert_equal(service.Back(), -ENOENT);
}

ZTEST(navigation_service, test_group_navigation_is_rejected_while_an_overlay_is_active) {
    NavigationService service;
    Configure(service, { 0, 1, 2 }, 0);

    zassert_equal(service.ShowOverlay(5), 0);
    zassert_true(service.IsOverlayActive());

    zassert_equal(service.Next(), -EBUSY);
    zassert_equal(service.Previous(), -EBUSY);
    zassert_equal(service.GoToGroup(2), -EBUSY);
    zassert_equal(ActiveGroupId(service), 0U);
}

ZTEST(navigation_service, test_CloseOverlay_releases_group_navigation) {
    NavigationService service;
    Configure(service, { 0, 1, 2 }, 0);

    zassert_equal(service.ShowOverlay(5), 0);
    zassert_equal(service.CloseOverlay(), 0);
    zassert_false(service.IsOverlayActive());

    zassert_equal(service.Next(), 0);
    zassert_equal(ActiveGroupId(service), 1U);
}

ZTEST(navigation_service, test_CloseOverlay_without_an_overlay_reports_ENOENT) {
    NavigationService service;
    Configure(service, { 0, 1 }, 0);

    zassert_equal(service.CloseOverlay(), -ENOENT);
    zassert_false(service.IsOverlayActive());
}

// Only one overlay is tracked, so a second one would leave a single CloseOverlay
// unblocking group navigation with an overlay still up.
ZTEST(navigation_service, test_a_second_overlay_is_rejected) {
    NavigationService service;
    NavigationProbe probe;
    zassert_true(probe.IsSubscribed());

    Configure(service, { 0, 1 }, 0);

    zassert_equal(service.ShowOverlay(5), 0);
    zassert_true(probe.WaitForEvent(), "ShowOverlay was not dispatched.");

    zassert_equal(service.ShowOverlay(6), -EBUSY);

    // A rejected request must not reach the view, or it would push a second overlay
    // that nothing tracks.
    zassert_false(probe.WaitForEvent(), "The rejected overlay was dispatched.");
    zassert_equal(probe.Count(), 1U);
    zassert_equal(probe.TargetScreenIds().front(), 5U);

    // The first overlay still owns the state, so closing it releases navigation.
    zassert_equal(service.CloseOverlay(), 0);
    zassert_false(service.IsOverlayActive());
    zassert_equal(service.Next(), 0);
}

ZTEST(navigation_service, test_Handle_routes_every_intent) {
    NavigationService service;
    Configure(service, { 0, 1, 2 }, 0);

    zassert_equal(service.Handle(NavigationIntent::NextGroup), 0);
    zassert_equal(ActiveGroupId(service), 1U);

    zassert_equal(service.Handle(NavigationIntent::PreviousGroup), 0);
    zassert_equal(ActiveGroupId(service), 0U);

    zassert_equal(service.Handle(NavigationIntent::GoToGroup, 2), 0);
    zassert_equal(ActiveGroupId(service), 2U);

    zassert_equal(service.Handle(NavigationIntent::Back), 0);
    zassert_equal(ActiveGroupId(service), 0U);

    zassert_equal(service.Handle(NavigationIntent::ShowOverlay, 3), 0);
    zassert_true(service.IsOverlayActive());

    zassert_equal(service.Handle(NavigationIntent::CloseOverlay), 0);
    zassert_false(service.IsOverlayActive());
}

ZTEST(navigation_service, test_Handle_rejects_an_empty_intent) {
    NavigationService service;
    Configure(service, { 0, 1 }, 0);

    zassert_equal(service.Handle(NavigationIntent::None), -EINVAL);
    zassert_equal(ActiveGroupId(service), 0U);
}

ZTEST(navigation_service, test_SetActiveGroupId_reconciles_the_service_with_the_view) {
    NavigationService service;
    Configure(service, { 0, 1, 2 }, 0);

    zassert_equal(service.Next(), 0);
    zassert_equal(ActiveGroupId(service), 1U);

    // The view refused the change and reported where it actually is.
    service.SetActiveGroupId(0);
    zassert_equal(ActiveGroupId(service), 0U);

    zassert_equal(service.Next(), 0);
    zassert_equal(ActiveGroupId(service), 1U);
}

ZTEST(navigation_service, test_a_group_change_publishes_a_ShowGroup_event) {
    NavigationService service;
    NavigationProbe probe;
    zassert_true(probe.IsSubscribed());

    Configure(service, { 0, 1, 2 }, 0);

    zassert_equal(service.GoToGroup(2), 0);

    zassert_true(probe.WaitForEvent(), "Navigation event was not dispatched.");
    zassert_equal(probe.Count(), 1U);
    zassert_equal(probe.Actions().front(), NavigationAction::ShowGroup);
    zassert_equal(probe.TargetGroupIds().front(), 2U);
}

ZTEST(navigation_service, test_a_rejected_request_publishes_nothing) {
    NavigationService service;
    NavigationProbe probe;
    zassert_true(probe.IsSubscribed());

    Configure(service, { 0, 1, 2 }, 0);

    zassert_equal(service.GoToGroup(9), -ENOENT);

    zassert_false(probe.WaitForEvent(), "No navigation event is expected.");
    zassert_equal(probe.Count(), 0U);
}

ZTEST(navigation_service, test_the_overlay_transitions_publish_the_target_screen) {
    NavigationService service;
    NavigationProbe probe;
    zassert_true(probe.IsSubscribed());

    Configure(service, { 0, 1 }, 0);

    zassert_equal(service.ShowOverlay(42), 0);
    zassert_true(probe.WaitForEvent(), "ShowOverlay was not dispatched.");

    zassert_equal(service.CloseOverlay(), 0);
    zassert_true(probe.WaitForEvent(), "CloseOverlay was not dispatched.");

    zassert_equal(probe.Count(), 2U);
    zassert_equal(probe.Actions().front(), NavigationAction::ShowOverlay);
    zassert_equal(probe.Actions().back(), NavigationAction::CloseOverlay);
    zassert_equal(probe.TargetScreenIds().front(), 42U);
    zassert_equal(probe.TargetScreenIds().back(), 42U);
}
