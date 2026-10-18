#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/event_bus/event_bus.h"
#include "subsys/event_bus/event_channel.h"
#include "subsys/threading/work_queue_thread.h"

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/string/string_helpers.h"

#include "domain/settings_domain/event_bus/settings_events_channel.h"
#include "domain/settings_domain/models/setting_id.h"
#include "domain/settings_domain/services/settings_persistence_service.h"

using eerie_leap::domain::settings_domain::event_bus::SettingsEventsChannel;
using eerie_leap::domain::settings_domain::event_bus::SettingsEventType;
using eerie_leap::domain::settings_domain::event_bus::SettingsPayloadType;
using eerie_leap::domain::settings_domain::models::SettingId;
using eerie_leap::domain::settings_domain::services::SettingsPersistenceService;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::event_bus::EventBus;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::utilities::string::StringHelpers;

namespace {

// The debounce is 1000 ms, so a request must be given more than that to arrive and appreciably
// less than that to prove it has not fired yet.
constexpr int DEBOUNCE_MS = 1000;
constexpr int DISPATCH_TIMEOUT_MS = 3000;
constexpr int NO_DISPATCH_TIMEOUT_MS = 400;
constexpr int BUS_STACK_SIZE = 4096;
constexpr int WORK_QUEUE_STACK_SIZE = 4096;
constexpr int WORK_QUEUE_PRIORITY = 10;

// SettingsEventsChannel is inert until a bus drains it, so the suite owns one.
class TestEventBus : public EventBus {
public:
    TestEventBus() :
        EventBus("settings_test_bus", BUS_STACK_SIZE) {
        RegisterChannel(SettingsEventsChannel::GetInstance());
    }
};

// Records the setting ids carried by every PersistRequested it sees.
class PersistProbe {
public:
    PersistProbe() :
        state_(std::make_shared<State>()) {
        k_sem_init(&state_->delivered, 0, K_SEM_MAX_LIMIT);

        subscription_ = CreateScopedSubscription(
            SettingsEventsChannel::GetInstance(), SettingsEventType::PersistRequested,
            [state = state_](const SettingsEventsChannel::EventMessage& event) {
                auto it = event.payload.find(SettingsPayloadType::SettingId);
                if(it != event.payload.end()) {
                    if(const auto* setting_id = std::get_if<uint32_t>(&it->second))
                        state->setting_ids.push_back(*setting_id);
                }

                k_sem_give(&state->delivered);
            }
        );
    }

    [[nodiscard]] bool IsSubscribed() const {
        return subscription_ != nullptr;
    }
    bool WaitForEvent() const {
        return k_sem_take(&state_->delivered, K_MSEC(DISPATCH_TIMEOUT_MS)) == 0;
    }
    bool WaitForNoEvent() const {
        return k_sem_take(&state_->delivered, K_MSEC(NO_DISPATCH_TIMEOUT_MS)) != 0;
    }

    [[nodiscard]] size_t Count() const {
        return state_->setting_ids.size();
    }
    [[nodiscard]] const std::vector<uint32_t>& SettingIds() const {
        return state_->setting_ids;
    }

private:
    struct State {
        std::vector<uint32_t> setting_ids;
        k_sem delivered{};
    };

    std::shared_ptr<State> state_;
    AnySubscription subscription_;
};

void PublishChanged(uint32_t setting_id) {
    SettingsEventsChannel::GetInstance().PublishAsync(
        { .source_id = 0,
            .type = SettingsEventType::Changed,
            .payload = { { SettingsPayloadType::SettingId, setting_id } } }
    );
}

uint32_t BrightnessId() {
    return StringHelpers::GetHash(SettingId::DISPLAY_BRIGHTNESS);
}

// One thread for the whole suite. Its stack comes from the external PMR rather than
// k_thread_stack_alloc, which needs CONFIG_DYNAMIC_THREAD - the same choice EventBus makes.
std::shared_ptr<WorkQueueThread> SharedWorkQueueThread() {
    static std::shared_ptr<WorkQueueThread> thread = [] {
        auto created = std::make_shared<WorkQueueThread>(
            "settings_persistence_test", WORK_QUEUE_STACK_SIZE, WORK_QUEUE_PRIORITY, false, Mrm::GetExtPmr()
        );

        created->Initialize();

        return created;
    }();

    return thread;
}

void* StartEventBus() {
    static TestEventBus bus;

    return nullptr;
}

// Lets anything the previous test published drain while no probe is subscribed, so a late
// request cannot be counted against the next one.
void SettleEventBus(void*) {
    k_msleep(DEBOUNCE_MS + 200);
}

} // namespace

ZTEST_SUITE(settings_persistence_service, NULL, StartEventBus, SettleEventBus, NULL, NULL);

ZTEST(settings_persistence_service, test_change_requests_a_persist_after_the_debounce) {
    auto work_queue_thread = SharedWorkQueueThread();

    SettingsPersistenceService service(work_queue_thread);
    zassert_equal(service.Initialize(), 0);

    PersistProbe probe;
    zassert_true(probe.IsSubscribed());

    PublishChanged(BrightnessId());

    zassert_true(probe.WaitForEvent(), "Expected a persist request.");
    zassert_equal(probe.Count(), 1U);
    zassert_equal(probe.SettingIds().front(), BrightnessId());
}

ZTEST(settings_persistence_service, test_persist_is_not_requested_before_the_debounce_elapses) {
    auto work_queue_thread = SharedWorkQueueThread();

    SettingsPersistenceService service(work_queue_thread);
    zassert_equal(service.Initialize(), 0);

    PersistProbe probe;

    PublishChanged(BrightnessId());

    zassert_true(probe.WaitForNoEvent(), "Expected the debounce to withhold the request.");
    zassert_equal(probe.Count(), 0U);
}

ZTEST(settings_persistence_service, test_a_burst_of_changes_collapses_into_one_request) {
    auto work_queue_thread = SharedWorkQueueThread();

    SettingsPersistenceService service(work_queue_thread);
    zassert_equal(service.Initialize(), 0);

    PersistProbe probe;

    for(int i = 0; i < 5; ++i) {
        PublishChanged(BrightnessId());
        k_msleep(50);
    }

    zassert_true(probe.WaitForEvent(), "Expected a persist request.");

    // Anything still pending would arrive well inside this window.
    zassert_true(probe.WaitForNoEvent(), "Expected the burst to collapse into a single request.");
    zassert_equal(probe.Count(), 1U);
}

ZTEST(settings_persistence_service, test_each_changed_setting_is_requested_once) {
    auto work_queue_thread = SharedWorkQueueThread();

    SettingsPersistenceService service(work_queue_thread);
    zassert_equal(service.Initialize(), 0);

    PersistProbe probe;

    auto other_id = StringHelpers::GetHash("other.setting");

    PublishChanged(BrightnessId());
    PublishChanged(other_id);
    PublishChanged(BrightnessId());

    zassert_true(probe.WaitForEvent(), "Expected a persist request.");
    zassert_true(probe.WaitForEvent(), "Expected a second persist request.");
    zassert_true(probe.WaitForNoEvent(), "Expected exactly one request per setting.");

    zassert_equal(probe.Count(), 2U);
    zassert_true(probe.SettingIds()[0] != probe.SettingIds()[1], "Expected two distinct settings.");
}

ZTEST(settings_persistence_service, test_a_change_without_a_setting_id_is_ignored) {
    auto work_queue_thread = SharedWorkQueueThread();

    SettingsPersistenceService service(work_queue_thread);
    zassert_equal(service.Initialize(), 0);

    PersistProbe probe;

    SettingsEventsChannel::GetInstance().PublishAsync(
        { .source_id = 0, .type = SettingsEventType::Changed, .payload = { { SettingsPayloadType::Value, 42 } } }
    );

    zassert_true(probe.WaitForNoEvent(), "Expected an unidentified change to be dropped.");
    zassert_equal(probe.Count(), 0U);
}

ZTEST(settings_persistence_service, test_a_service_without_a_work_queue_does_not_initialize) {
    SettingsPersistenceService service(nullptr);

    zassert_not_equal(service.Initialize(), 0);
}

// The service unsubscribes and cancels its pending work in its destructor; a request that
// outlived it would reach a dangling handler.
ZTEST(settings_persistence_service, test_a_destroyed_service_stops_requesting) {
    auto work_queue_thread = SharedWorkQueueThread();

    PersistProbe probe;

    {
        SettingsPersistenceService service(work_queue_thread);
        zassert_equal(service.Initialize(), 0);

        PublishChanged(BrightnessId());
        k_msleep(50);
    }

    zassert_true(probe.WaitForNoEvent(), "Expected no request from a destroyed service.");
    k_msleep(DEBOUNCE_MS);
    zassert_equal(probe.Count(), 0U);
}
