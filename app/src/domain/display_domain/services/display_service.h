#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <zephyr/kernel.h>

#include "subsys/event_bus/dispatch_guard.h"
#include "subsys/event_bus/event.h"
#include "subsys/event_bus/i_scoped_subscription.h"
#include "subsys/threading/work_queue_task.h"
#include "subsys/threading/work_queue_task_result.h"
#include "subsys/threading/work_queue_thread.h"

#include "domain/display_domain/configuration/display_configuration_manager.h"
#include "domain/display_domain/models/display_configuration.h"
#include "domain/settings_domain/event_bus/settings_events_channel.h"

namespace eerie_leap::domain::display_domain::services {

using eerie_leap::domain::display_domain::configuration::DisplayConfigurationManager;
using eerie_leap::domain::display_domain::models::DisplayConfiguration;
using eerie_leap::domain::settings_domain::event_bus::SettingsEventsChannel;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::DispatchGuard;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::subsys::threading::WorkQueueTask;
using eerie_leap::subsys::threading::WorkQueueTaskResult;
using eerie_leap::subsys::threading::WorkQueueThread;

// Owns the panel state (brightness, blanking) and its persistence. The only
// place in the app that talks to the display driver outside of LVGL's own
// flush path.
//
// The brightness on the panel can differ from the one in the configuration. A manual change (a
// request on the settings channel) sets both and is written at the next persist. A sensor the
// configuration names sets the panel only, so a value that keeps changing costs no flash write, and
// the stored brightness is what the unit starts with. The latest change wins.
class DisplayService {
private:
    struct PersistTask {
        DisplayService* service = nullptr;
    };

    // The floor is deliberately above 0: a slider that can black the panel out leaves no way
    // to find the slider again.
    static constexpr double BRIGHTNESS_MIN = 10;
    static constexpr double BRIGHTNESS_MAX = 255;
    static constexpr double BRIGHTNESS_STEP = 5;

    std::shared_ptr<DisplayConfigurationManager> configuration_manager_;
    std::shared_ptr<WorkQueueThread> config_work_queue_thread_;

    // What the unit stores: its brightness is the manual one.
    DisplayConfiguration configuration_;
    // What the panel shows: the manual brightness until a followed sensor's value replaces it.
    uint8_t panel_brightness_;
    // configuration_ holds a manual change that is not on flash yet.
    bool is_marked_for_write_ = false;
    mutable k_mutex lock_{};

    std::optional<WorkQueueTask<PersistTask>> persist_task_;

    AnySubscription change_requested_subscription_;
    AnySubscription state_requested_subscription_;
    AnySubscription persist_requested_subscription_;

    // The sensor being followed, none when empty. Changed by Initialize() and Reload() only, which
    // run one at a time (the latter on the configuration work queue), never under lock_: Detach()
    // waits for a value in progress, and that value takes lock_.
    std::string followed_sensor_id_;
    std::shared_ptr<DispatchGuard> brightness_sensor_guard_;
    AnySubscription brightness_sensor_subscription_;

    bool is_initialized_ = false;

    int ApplyBrightness(uint8_t value) const;
    int ApplyBlanking(bool enabled) const;

    static uint8_t ClampBrightness(double requested);

    void SubscribeSettings();
    void OnBrightnessChangeRequested(const SettingsEventsChannel::EventMessage& event);
    void PublishBrightnessValue() const;
    static void PublishBrightnessRange();

    void FollowBrightnessSensor(std::string_view sensor_id);
    void StopFollowingBrightnessSensor();
    void OnBrightnessSensorValue(const EventData& value);

    static WorkQueueTaskResult PersistWork(PersistTask* task);
    void WriteConfiguration();

public:
    DisplayService(
        std::shared_ptr<DisplayConfigurationManager> configuration_manager,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread
    );

    ~DisplayService();

    DisplayService(const DisplayService&) = delete;
    DisplayService& operator=(const DisplayService&) = delete;

    int Initialize();
    bool IsInitialized() const;

    // A manual change: the panel and the stored brightness, written at the next Persist().
    int SetBrightness(uint8_t value);
    // What the panel shows.
    uint8_t GetBrightness() const;

    int SetBlanking(bool enabled);
    bool IsBlankingEnabled() const;

    // Re-reads the stored configuration and pushes it to the panel; used when a
    // configuration arrives from outside (BLE) rather than from a control.
    int Reload();

    // Hands the flash write to the configuration work queue: this is reached from the event
    // bus worker, which must not block. Repeated calls coalesce into a single write, and a
    // configuration with no manual change waiting is not written at all.
    int Persist();
};

} // namespace eerie_leap::domain::display_domain::services
