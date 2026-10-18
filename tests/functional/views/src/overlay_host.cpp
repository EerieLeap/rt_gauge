#include <cerrno>
#include <cstdint>
#include <memory>
#include <stdexcept>

#include <lvgl.h>
#include <zephyr/ztest.h>

#include "domain/ui_domain/services/navigation_service.h"

#include "views/overlay_host.h"
#include "views/screens/screen_group.h"
#include "views/utilitites/frame.h"

#include "views_test_support.h"

using eerie_leap::domain::ui_domain::services::NavigationService;
using eerie_leap::views::OverlayHost;
using eerie_leap::views::OverlayOptions;
using eerie_leap::views::screens::ScreenGroup;
using eerie_leap::views::utilitites::Frame;
using views_test::CleanTestDisplay;
using views_test::EnsureTestDisplay;
using views_test::FakeScreen;

namespace {

constexpr uint32_t overlay_screen_id = 7;

// Pop() destroys the group and every screen in it, so a test that asserts on a
// dismissed overlay has to outlive the screen with its own counter.
class CountingScreen : public FakeScreen {
private:
    std::shared_ptr<int> deactivations_;

public:
    CountingScreen(uint32_t id, std::shared_ptr<Frame> parent, std::shared_ptr<int> deactivations) :
        FakeScreen(id, id, 0, true, std::move(parent)),
        deactivations_(std::move(deactivations)) {}

    void OnDeactivated() override {
        FakeScreen::OnDeactivated();

        (*deactivations_)++;
    }
};

// Activation reaches widget code, which can throw - a failed push still has to
// leave the host and the top layer as it found them.
class ThrowingScreen : public FakeScreen {
public:
    using FakeScreen::FakeScreen;

    void OnActivated() override {
        FakeScreen::OnActivated();

        throw std::runtime_error("activation failed");
    }
};

// The scrim and the auto-close timer both close through NavigationService, whose
// overlay state is what a test can observe without a running event bus.
std::shared_ptr<NavigationService> MakeNavigationWithOverlay() {
    auto navigation_service = std::make_shared<NavigationService>();
    navigation_service->ShowOverlay(overlay_screen_id);

    return navigation_service;
}

std::shared_ptr<OverlayHost> MakeHost(std::shared_ptr<NavigationService> navigation_service = nullptr) {
    return std::make_shared<OverlayHost>(std::move(navigation_service));
}

struct Overlay {
    std::unique_ptr<ScreenGroup> screen_group;
    std::shared_ptr<FakeScreen> screen;
    std::shared_ptr<int> deactivations;
};

// A screen is never rendered on its own, so every overlay is a group of one here.
Overlay MakeOverlay(const std::shared_ptr<OverlayHost>& host, uint32_t id) {
    auto deactivations = std::make_shared<int>(0);
    auto screen_group = host->CreateGroup(id);
    auto screen = std::make_shared<CountingScreen>(id, screen_group->GetContainer(), deactivations);

    screen_group->AddScreen(screen);

    return Overlay{ std::move(screen_group), std::move(screen), std::move(deactivations) };
}

int PushOverlay(const std::shared_ptr<OverlayHost>& host, uint32_t id, const OverlayOptions& options = {}) {

    auto overlay = MakeOverlay(host, id);

    return host->Push(std::move(overlay.screen_group), options);
}

bool IsHidden(const std::shared_ptr<OverlayHost>& host) {
    return lv_obj_has_flag(host->GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN);
}

uint32_t ChildCount(const std::shared_ptr<OverlayHost>& host) {
    return lv_obj_get_child_count(host->GetContainer()->GetObject());
}

// LVGL only runs timers from lv_timer_handler(), and only once their period has
// elapsed. The suites do not run Zephyr's LVGL init, so the tick may still be
// LVGL's own counter rather than k_uptime_get_32(); advance both.
void RunTimersFor(uint32_t duration_ms) {
    k_msleep(duration_ms);
    lv_tick_inc(duration_ms);
    lv_timer_handler();
}

void* SetUp() {
    EnsureTestDisplay();

    return nullptr;
}

} // namespace

ZTEST_SUITE(overlay_host, NULL, SetUp, NULL, CleanTestDisplay, NULL);

ZTEST(overlay_host, test_a_new_host_is_empty_and_hidden) {
    auto host = MakeHost();

    zassert_true(host->IsEmpty());
    zassert_equal(host->GetDepth(), 0);
    zassert_true(host->GetTopGroup() == nullptr);
    zassert_true(IsHidden(host));
}

// The host sits on the top layer so it paints above every screen group.
ZTEST(overlay_host, test_the_container_lives_on_the_top_layer) {
    auto host = MakeHost();

    zassert_equal(lv_obj_get_parent(host->GetContainer()->GetObject()), lv_layer_top());
}

// A full-screen clickable container would swallow the swipes navigation runs on,
// so blocking input has to be the scrim's job alone.
ZTEST(overlay_host, test_the_container_does_not_take_input) {
    auto host = MakeHost();

    zassert_false(lv_obj_has_flag(host->GetContainer()->GetObject(), LV_OBJ_FLAG_CLICKABLE));
}

ZTEST(overlay_host, test_push_renders_activates_and_shows) {
    auto host = MakeHost();
    auto overlay = MakeOverlay(host, 1);

    zassert_ok(host->Push(std::move(overlay.screen_group)));

    zassert_equal(overlay.screen->render_count, 1);
    zassert_equal(overlay.screen->activated_count, 1);
    zassert_equal(host->GetDepth(), 1);
    zassert_equal(host->GetTopGroup()->GetGroupId(), 1);
    zassert_false(IsHidden(host));
}

ZTEST(overlay_host, test_push_rejects_a_missing_group) {
    auto host = MakeHost();

    zassert_equal(host->Push(nullptr), -EINVAL);
    zassert_true(host->IsEmpty());
}

// A group with nothing in it would show as a bare scrim.
ZTEST(overlay_host, test_push_rejects_an_empty_group) {
    auto host = MakeHost();

    zassert_equal(host->Push(host->CreateGroup(1)), -EINVAL);
    zassert_true(host->IsEmpty());
}

ZTEST(overlay_host, test_push_refuses_to_stack_without_bound) {
    auto host = MakeHost();

    int pushed = 0;
    for(uint32_t id = 0; id < 16; id++) {
        if(PushOverlay(host, id) != 0)
            break;

        pushed++;
    }

    zassert_true(pushed > 0);
    zassert_equal(host->GetDepth(), static_cast<size_t>(pushed));
    zassert_equal(PushOverlay(host, 99), -ENOSPC);
}

// A modal overlay needs the scrim under it, not over it.
ZTEST(overlay_host, test_a_modal_push_puts_the_group_above_its_scrim) {
    auto host = MakeHost();

    zassert_ok(PushOverlay(host, 1, OverlayOptions{ .is_modal = true }));

    zassert_equal(ChildCount(host), 2);

    auto* group_object = host->GetTopGroup()->GetContainer()->GetObject();
    zassert_equal(lv_obj_get_index(group_object), 1);

    auto* scrim = lv_obj_get_child(host->GetContainer()->GetObject(), 0);
    zassert_not_equal(scrim, group_object);
    zassert_true(lv_obj_has_flag(scrim, LV_OBJ_FLAG_CLICKABLE));
}

ZTEST(overlay_host, test_a_non_modal_push_has_no_scrim) {
    auto host = MakeHost();

    zassert_ok(PushOverlay(host, 1, OverlayOptions{ .is_modal = false }));

    zassert_equal(ChildCount(host), 1);
}

ZTEST(overlay_host, test_a_failing_screen_leaves_the_host_empty) {
    auto host = MakeHost();
    auto overlay = MakeOverlay(host, 1);
    overlay.screen->FailNextRenders();

    zassert_not_equal(host->Push(std::move(overlay.screen_group)), 0);

    zassert_true(host->IsEmpty());
    zassert_true(IsHidden(host));
    zassert_equal(overlay.screen->activated_count, 0);
    zassert_equal(overlay.screen->deactivated_count, 0);

    // A screen holds the frame it was built on, which is the group's container.
    overlay.screen.reset();

    // The push was modal by default, so this also says the scrim was taken back.
    zassert_equal(ChildCount(host), 0);
}

ZTEST(overlay_host, test_a_screen_that_throws_while_activating_leaves_the_host_empty) {
    auto host = MakeHost();
    auto screen_group = host->CreateGroup(1);
    screen_group->AddScreen(std::make_shared<ThrowingScreen>(1, 1, 0, true, screen_group->GetContainer()));

    zassert_not_equal(host->Push(std::move(screen_group)), 0);

    zassert_true(host->IsEmpty());
    zassert_true(IsHidden(host));
    zassert_equal(ChildCount(host), 0);
}

ZTEST(overlay_host, test_pop_deactivates_and_hides) {
    auto host = MakeHost();
    auto overlay = MakeOverlay(host, 1);

    zassert_ok(host->Push(std::move(overlay.screen_group)));

    // The host owns the screen now; keeping it would outlive the objects Pop() frees.
    overlay.screen.reset();

    zassert_ok(host->Pop());

    zassert_equal(*overlay.deactivations, 1);
    zassert_true(host->IsEmpty());
    zassert_true(IsHidden(host));
    zassert_equal(ChildCount(host), 0);
}

ZTEST(overlay_host, test_pop_on_an_empty_host_is_reported) {
    auto host = MakeHost();

    zassert_equal(host->Pop(), -ENOENT);
}

ZTEST(overlay_host, test_pop_removes_only_the_top_overlay) {
    auto host = MakeHost();
    auto bottom = MakeOverlay(host, 1);
    auto top = MakeOverlay(host, 2);

    zassert_ok(host->Push(std::move(bottom.screen_group)));
    zassert_ok(host->Push(std::move(top.screen_group)));
    zassert_equal(host->GetTopGroup()->GetGroupId(), 2);

    bottom.screen.reset();
    top.screen.reset();

    zassert_ok(host->Pop());

    zassert_equal(host->GetDepth(), 1);
    zassert_equal(host->GetTopGroup()->GetGroupId(), 1);
    zassert_equal(*top.deactivations, 1);
    zassert_equal(*bottom.deactivations, 0);
    zassert_false(IsHidden(host));
}

ZTEST(overlay_host, test_dismiss_all_clears_the_stack) {
    auto host = MakeHost();
    auto first = MakeOverlay(host, 1);
    auto second = MakeOverlay(host, 2);

    zassert_ok(host->Push(std::move(first.screen_group)));
    zassert_ok(host->Push(std::move(second.screen_group)));

    first.screen.reset();
    second.screen.reset();

    host->DismissAll();

    zassert_true(host->IsEmpty());
    zassert_true(IsHidden(host));
    zassert_equal(*first.deactivations, 1);
    zassert_equal(*second.deactivations, 1);
    zassert_equal(ChildCount(host), 0);
}

// Nothing but the host holds the screen_group, so popping takes its objects with it.
ZTEST(overlay_host, test_pop_releases_the_overlays_objects) {
    auto host = MakeHost();

    zassert_ok(PushOverlay(host, 1));
    zassert_equal(ChildCount(host), 2);

    zassert_ok(host->Pop());

    zassert_equal(ChildCount(host), 0);
}

// Nothing else deletes the container, and the top layer outlives the host.
ZTEST(overlay_host, test_destruction_leaves_nothing_on_the_top_layer) {
    uint32_t before = lv_obj_get_child_count(lv_layer_top());

    {
        auto host = MakeHost();
        zassert_ok(PushOverlay(host, 1));
        zassert_equal(lv_obj_get_child_count(lv_layer_top()), before + 1);
    }

    zassert_equal(lv_obj_get_child_count(lv_layer_top()), before);
}

ZTEST(overlay_host, test_a_scrim_tap_closes_through_navigation) {
    auto navigation_service = MakeNavigationWithOverlay();
    auto host = MakeHost(navigation_service);

    zassert_ok(PushOverlay(host, 1));
    zassert_true(navigation_service->IsOverlayActive());

    lv_obj_send_event(lv_obj_get_child(host->GetContainer()->GetObject(), 0), LV_EVENT_CLICKED, nullptr);

    zassert_false(navigation_service->IsOverlayActive());

    // Closing is the navigation service's decision to route back as a Pop().
    zassert_equal(host->GetDepth(), 1);
}

ZTEST(overlay_host, test_a_scrim_tap_is_ignored_when_it_cannot_dismiss) {
    auto navigation_service = MakeNavigationWithOverlay();
    auto host = MakeHost(navigation_service);

    zassert_ok(PushOverlay(host, 1, OverlayOptions{ .close_on_scrim_tap = false }));

    lv_obj_send_event(lv_obj_get_child(host->GetContainer()->GetObject(), 0), LV_EVENT_CLICKED, nullptr);

    zassert_true(navigation_service->IsOverlayActive());
}

ZTEST(overlay_host, test_an_overlay_closes_itself_when_its_time_is_up) {
    auto navigation_service = MakeNavigationWithOverlay();
    auto host = MakeHost(navigation_service);

    zassert_ok(PushOverlay(host, 1, OverlayOptions{ .auto_close_ms = 10 }));
    zassert_true(navigation_service->IsOverlayActive());

    RunTimersFor(40);

    zassert_false(navigation_service->IsOverlayActive());
}

// A single-shot timer outliving the overlay it belongs to would close whatever
// took its place - or run against a destroyed host.
ZTEST(overlay_host, test_popping_cancels_a_pending_auto_close) {
    auto navigation_service = MakeNavigationWithOverlay();
    auto host = MakeHost(navigation_service);

    zassert_ok(PushOverlay(host, 1, OverlayOptions{ .auto_close_ms = 10 }));
    zassert_ok(host->Pop());

    RunTimersFor(40);

    zassert_true(navigation_service->IsOverlayActive());
}

ZTEST(overlay_host, test_an_overlay_stacked_on_top_takes_over_closing) {
    auto navigation_service = MakeNavigationWithOverlay();
    auto host = MakeHost(navigation_service);

    zassert_ok(PushOverlay(host, 1, OverlayOptions{ .auto_close_ms = 10 }));
    zassert_ok(PushOverlay(host, 2));

    RunTimersFor(40);

    // The elapsed timer belonged to the overlay underneath, which is not what a
    // close would remove.
    zassert_true(navigation_service->IsOverlayActive());
    zassert_equal(host->GetDepth(), 2);
}
