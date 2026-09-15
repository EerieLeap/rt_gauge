#include <cstdint>
#include <memory>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "subsys/device_tree/dt_display.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/event_bus/event_bus.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/threading/work_queue_thread.h"

#include "domain/settings_domain/event_bus/settings_event_type.h"
#include "domain/settings_domain/event_bus/settings_events_channel.h"
#include "domain/ui_domain/event_bus/navigation_event_channel.h"
#include "domain/ui_domain/services/navigation_service.h"

#include "controllers/ui_controller.h"

using eerie_leap::controllers::UiController;
using eerie_leap::domain::settings_domain::event_bus::SettingsEventsChannel;
using eerie_leap::domain::settings_domain::event_bus::SettingsEventType;
using eerie_leap::domain::ui_domain::event_bus::NavigationEventChannel;
using eerie_leap::domain::ui_domain::services::NavigationService;
using eerie_leap::subsys::device_tree::DtDisplay;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::event_bus::EventBus;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::threading::WorkQueueThread;

namespace {

constexpr int BUS_STACK_SIZE = 4096;
constexpr int CONFIG_WORK_QUEUE_STACK_SIZE = 8192;
constexpr int CONFIG_WORK_QUEUE_PRIORITY = 10;

// The demo configuration UiController installs on every Initialize().
constexpr uint32_t DEMO_OVERLAY_SCREEN_ID = 3;
constexpr uint32_t UNKNOWN_SCREEN_ID = 99;
constexpr uint32_t FIRST_GROUP_ID = 0;

// A navigation change is applied on the bus worker; give it room to run and, for
// the reconcile paths, to publish the follow-up close and apply that too.
constexpr int SETTLE_MS = 300;
constexpr int DISPATCH_TIMEOUT_MS = 1000;

// UiController subscribes to NavigationEventChannel, which stays inert until a
// bus drains it.
class ControllerBus : public EventBus {
public:
    ControllerBus() : EventBus("ctrl_test_bus", BUS_STACK_SIZE) {
        RegisterChannel(NavigationEventChannel::GetInstance());
        RegisterChannel(SettingsEventsChannel::GetInstance());
    }
};

// One controller for the whole suite: Initialize() builds every screen in the demo
// configuration and writes ~870 KB of assets, and its LVGL objects are process-wide.
struct Fixture {
    std::shared_ptr<WorkQueueThread> config_work_queue;
    std::shared_ptr<UiController> controller;
    std::shared_ptr<NavigationService> navigation_service;
};

Fixture fixture;

void* Setup() {
    static ControllerBus bus;

    // UiRendererService reads the display through DtDisplay, which main() primes.
    DtDisplay::Initialize();

    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    fs_service->Format();

    fixture.config_work_queue = std::make_shared<WorkQueueThread>(
        "cfg_work_q", CONFIG_WORK_QUEUE_STACK_SIZE, CONFIG_WORK_QUEUE_PRIORITY);
    zassert_true(fixture.config_work_queue->Initialize(), "Expected the config work queue to start.");

    fixture.controller = std::make_shared<UiController>(
        fs_service, fixture.config_work_queue, nullptr, nullptr);

    zassert_equal(fixture.controller->Initialize(), 0, "Expected the UI controller to initialize.");
    zassert_equal(fixture.controller->Start(), 0, "Expected the UI controller to start.");

    fixture.navigation_service = fixture.controller->GetNavigationService();
    zassert_not_null(fixture.navigation_service.get(), "Expected a navigation service.");

    return nullptr;
}

// Overlay and group state are process-wide, so each test starts from the same place
// rather than from wherever the previous one left off.
void Reset(void*) {
    if(fixture.navigation_service->IsOverlayActive())
        fixture.navigation_service->CloseOverlay();

    k_msleep(SETTLE_MS);

    fixture.navigation_service->GoToGroup(FIRST_GROUP_ID);
    k_msleep(SETTLE_MS);
}

// Without this the renderer thread keeps the simulation alive after the report.
void Teardown(void*) {
    fixture.controller.reset();
    fixture.navigation_service.reset();
    fixture.config_work_queue.reset();
}

NavigationService& Navigation() { return *fixture.navigation_service; }

} // namespace

ZTEST_SUITE(ui_controller, NULL, Setup, Reset, Reset, Teardown);

// Configure() reads the stored configuration and must have found usable groups.
ZTEST(ui_controller, test_the_controller_starts_on_a_configured_screen_group) {
    auto active_screen_group_id = Navigation().GetActiveGroupId();

    zassert_true(active_screen_group_id.has_value(), "Expected an active screen group.");
    zassert_equal(*active_screen_group_id, FIRST_GROUP_ID);
    zassert_false(Navigation().IsOverlayActive());
}

// An overlay shares a screen_group_id with a real screen; Configure() must skip it rather
// than render it into that group, and navigation must still be able to move.
ZTEST(ui_controller, test_group_navigation_works_alongside_a_configured_overlay) {
    zassert_equal(Navigation().Next(), 0, "Expected the next group to be reachable.");
    k_msleep(SETTLE_MS);

    auto active_screen_group_id = Navigation().GetActiveGroupId();
    zassert_true(active_screen_group_id.has_value());
    zassert_not_equal(*active_screen_group_id, FIRST_GROUP_ID, "Expected the group to have changed.");

    zassert_equal(Navigation().GoToGroup(FIRST_GROUP_ID), 0);
    k_msleep(SETTLE_MS);
}

// The whole ShowOverlay path: event -> ScreenFactory -> OverlayHost::Push. Any
// failure along it reconciles by closing, so an overlay still active after the
// bus has settled is proof the push succeeded.
ZTEST(ui_controller, test_showing_a_configured_overlay_keeps_it_active) {
    zassert_equal(Navigation().ShowOverlay(DEMO_OVERLAY_SCREEN_ID), 0);
    k_msleep(SETTLE_MS);

    zassert_true(Navigation().IsOverlayActive(), "Expected the overlay to have been pushed.");
}

ZTEST(ui_controller, test_overlay_can_reopen_above_the_shape_screen) {
    // Group 1 contains the large rounded shape. Its initial render runs on the
    // UI work queue; opening the overlay then allocates and redraws above it.
    zassert_equal(Navigation().GoToGroup(1), 0);
    k_msleep(SETTLE_MS);

    for(int i = 0; i < 3; ++i) {
        zassert_equal(Navigation().ShowOverlay(DEMO_OVERLAY_SCREEN_ID), 0);
        k_msleep(SETTLE_MS);
        zassert_true(Navigation().IsOverlayActive());
        zassert_equal(Navigation().CloseOverlay(), 0);
        k_msleep(SETTLE_MS);
        zassert_false(Navigation().IsOverlayActive());
        zassert_equal(*Navigation().GetActiveGroupId(), 1);
    }
}

// An overlay is built long after Configure() broadcast the settings state, so the
// bindings its widgets just resolved have to be seeded again or they keep their defaults.
ZTEST(ui_controller, test_showing_an_overlay_asks_settings_owners_to_republish) {
    k_sem requested;
    k_sem_init(&requested, 0, K_SEM_MAX_LIMIT);

    AnySubscription subscription = CreateScopedSubscription(
        SettingsEventsChannel::GetInstance(),
        SettingsEventType::StateRequested,
        [&requested](const SettingsEventsChannel::EventMessage&) { k_sem_give(&requested); });
    zassert_not_null(subscription.get(), "Expected to subscribe to settings events.");

    zassert_equal(Navigation().ShowOverlay(DEMO_OVERLAY_SCREEN_ID), 0);

    zassert_equal(k_sem_take(&requested, K_MSEC(DISPATCH_TIMEOUT_MS)), 0,
        "Expected the overlay to request the settings state.");
}

ZTEST(ui_controller, test_an_active_overlay_blocks_group_navigation) {
    zassert_equal(Navigation().ShowOverlay(DEMO_OVERLAY_SCREEN_ID), 0);
    k_msleep(SETTLE_MS);

    zassert_equal(Navigation().Next(), -EBUSY);
    zassert_equal(Navigation().Previous(), -EBUSY);
    zassert_equal(Navigation().GoToGroup(FIRST_GROUP_ID), -EBUSY);
    zassert_equal(Navigation().ShowOverlay(DEMO_OVERLAY_SCREEN_ID), -EBUSY);
}

ZTEST(ui_controller, test_closing_an_overlay_restores_group_navigation) {
    zassert_equal(Navigation().ShowOverlay(DEMO_OVERLAY_SCREEN_ID), 0);
    k_msleep(SETTLE_MS);

    zassert_equal(Navigation().CloseOverlay(), 0);
    k_msleep(SETTLE_MS);

    zassert_false(Navigation().IsOverlayActive());
    zassert_equal(Navigation().Next(), 0, "Expected group navigation to work again.");
    k_msleep(SETTLE_MS);
}

// A screen id that is not a configured overlay: the service has already taken the
// optimistic state, so the controller has to hand it back or navigation stays stuck.
ZTEST(ui_controller, test_an_unknown_overlay_id_reconciles_navigation) {
    zassert_equal(Navigation().ShowOverlay(UNKNOWN_SCREEN_ID), 0);
    zassert_true(Navigation().IsOverlayActive(), "Expected the request to be taken optimistically.");

    k_msleep(SETTLE_MS);

    zassert_false(Navigation().IsOverlayActive(), "Expected the failed show to close itself.");
    zassert_equal(Navigation().Next(), 0, "Expected group navigation to be unblocked.");
    k_msleep(SETTLE_MS);
}

// A non-overlay screen must not be showable as one, even though its id exists.
ZTEST(ui_controller, test_a_non_overlay_screen_id_reconciles_navigation) {
    zassert_equal(Navigation().ShowOverlay(FIRST_GROUP_ID), 0);
    k_msleep(SETTLE_MS);

    zassert_false(Navigation().IsOverlayActive(), "Expected a plain screen to be refused as an overlay.");
}

// Nothing to close is reported as such, and navigation stays where it is.
ZTEST(ui_controller, test_closing_with_no_overlay_open_is_refused) {
    zassert_equal(Navigation().CloseOverlay(), -ENOENT);
    k_msleep(SETTLE_MS);

    zassert_false(Navigation().IsOverlayActive());
    zassert_true(Navigation().GetActiveGroupId().has_value());
}
