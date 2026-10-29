#include <algorithm>
#include <cerrno>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

#include "subsys/device_tree/dt_display.h"
#include "subsys/event_bus/event_binding.h"
#include "subsys/event_bus/event_binding_subscription.h"
#include "subsys/event_bus/event_channel_id.h"
#include "subsys/threading/scoped_mutex.h"
#include "utilities/memory/memory_resource_manager.h"
#include "utilities/reflection/caller_name.h"
#include "utilities/string/string_helpers.h"

#include "domain/sensor_domain/event_bus/sensor_event_type.h"
#include "domain/sensor_domain/event_bus/sensor_payload_type.h"
#include "domain/settings_domain/models/setting_id.h"
#include "domain/ui_domain/lvgl_lock.h"

#include "display_service.h"

namespace eerie_leap::domain::display_domain::services {

using eerie_leap::domain::sensor_domain::event_bus::SensorEventType;
using eerie_leap::domain::sensor_domain::event_bus::SensorPayloadType;
using eerie_leap::domain::settings_domain::models::SettingId;
using eerie_leap::domain::ui_domain::ScopedLvglLock;
using eerie_leap::subsys::device_tree::DtDisplay;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::event_bus::EventBinding;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::SubscribeEventBinding;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::utilities::reflection::GetCallerName;
using eerie_leap::utilities::string::StringHelpers;

using namespace eerie_leap::domain::settings_domain::event_bus;

LOG_MODULE_REGISTER(display_service_logger);

namespace {

uint32_t BrightnessSettingId() {
    static const uint32_t ID = StringHelpers::GetHash(SettingId::DISPLAY_BRIGHTNESS);

    return ID;
}

bool IsBrightnessSetting(const SettingsEventsChannel::EventMessage& event) {
    const auto* it = event.payload.find(SettingsPayloadType::SettingId);
    if(it == event.payload.end())
        return false;

    const auto* id = std::get_if<uint32_t>(&it->second);

    return id != nullptr && *id == BrightnessSettingId();
}

// A request that names no setting is addressed to every owner.
bool IsBrightnessStateRequest(const SettingsEventsChannel::EventMessage& event) {
    return !event.payload.contains(SettingsPayloadType::SettingId) || IsBrightnessSetting(event);
}

// An int or a float, which is what a request and a sensor carry. Text, a bool and NaN are no brightness.
std::optional<double> ReadNumber(const EventData& value) {
    if(const auto* int_value = std::get_if<int>(&value))
        return *int_value;

    if(const auto* float_value = std::get_if<float>(&value))
        return std::isnan(*float_value) ? std::nullopt : std::optional<double>(*float_value);

    return std::nullopt;
}

} // namespace

DisplayService::DisplayService(
    std::shared_ptr<DisplayConfigurationManager> configuration_manager,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread
) :
    configuration_manager_(std::move(configuration_manager)),
    config_work_queue_thread_(std::move(config_work_queue_thread)),
    panel_brightness_(configuration_.brightness) {

    k_mutex_init(&lock_);
}

DisplayService::~DisplayService() {
    StopFollowingBrightnessSensor();

    change_requested_subscription_.reset();
    state_requested_subscription_.reset();
    persist_requested_subscription_.reset();

    if(persist_task_.has_value())
        persist_task_->Cancel();
}

int DisplayService::Initialize() {
    const auto* display = DtDisplay::Get();
    if(display == nullptr || !device_is_ready(display)) {
        LOG_ERR("Display not ready, aborting.");
        return -ENODEV;
    }

    DisplayConfiguration snapshot;
    const auto configuration = configuration_manager_->Get();
    if(configuration != nullptr)
        snapshot = *configuration;
    else
        LOG_WRN("No stored Display configuration, using defaults.");

    is_initialized_ = true;

    {
        ScopedMutex guard(lock_);
        configuration_ = snapshot;
        panel_brightness_ = snapshot.brightness;
    }

    if(config_work_queue_thread_ != nullptr) {
        auto task = std::make_unique<PersistTask>();
        task->service = this;

        persist_task_ = config_work_queue_thread_->CreateTask(PersistWork, std::move(task));
    }

    ApplyBlanking(snapshot.blanking_enabled);
    ApplyBrightness(snapshot.brightness);

    SubscribeSettings();
    FollowBrightnessSensor(snapshot.brightness_sensor_id);

    LOG_INF("Display service initialized. Brightness: %u.", snapshot.brightness);

    return 0;
}

void DisplayService::SubscribeSettings() {
    change_requested_subscription_ = CreateScopedSubscription(
        SettingsEventsChannel::GetInstance(), SettingsEventType::ChangeRequested, IsBrightnessSetting,
        [this](const SettingsEventsChannel::EventMessage& event) {
            OnBrightnessChangeRequested(event);
        }
    );

    state_requested_subscription_ = CreateScopedSubscription(
        SettingsEventsChannel::GetInstance(), SettingsEventType::StateRequested, IsBrightnessStateRequest,
        [this](const SettingsEventsChannel::EventMessage&) {
            PublishBrightnessRange();
            PublishBrightnessValue();
        }
    );

    persist_requested_subscription_ = CreateScopedSubscription(
        SettingsEventsChannel::GetInstance(), SettingsEventType::PersistRequested, IsBrightnessSetting,
        [this](const SettingsEventsChannel::EventMessage&) {
            Persist();
        }
    );

    PublishBrightnessRange();
    PublishBrightnessValue();
}

void DisplayService::OnBrightnessChangeRequested(const SettingsEventsChannel::EventMessage& event) {
    const auto* it = event.payload.find(SettingsPayloadType::Value);
    if(it == event.payload.end())
        return;

    const auto requested = ReadNumber(it->second);
    if(!requested.has_value())
        return;

    const auto clamped = ClampBrightness(*requested);

    // Only a request that actually moved something is a fact worth waking every subscriber - and
    // worth re-arming the persistence debounce - for. A value equal to what a sensor put on the
    // panel still moves the stored brightness, which a manual change is.
    {
        ScopedMutex guard(lock_);

        if(clamped == panel_brightness_ && clamped == configuration_.brightness)
            return;
    }

    if(SetBrightness(clamped) != 0)
        return;

    PublishBrightnessValue();
}

uint8_t DisplayService::ClampBrightness(double requested) {
    return static_cast<uint8_t>(std::lround(std::clamp(requested, BRIGHTNESS_MIN, BRIGHTNESS_MAX)));
}

void DisplayService::PublishBrightnessValue() const {
    static constexpr auto CALLER = GetCallerName();

    SettingsEventsChannel::GetInstance().PublishAsync(
        { .source_id = CALLER.hash,
            .type = SettingsEventType::Changed,
            .payload = { { SettingsPayloadType::SettingId, BrightnessSettingId() },
                { SettingsPayloadType::Value, static_cast<int>(GetBrightness()) } } }
    );
}

void DisplayService::PublishBrightnessRange() {
    static constexpr auto CALLER = GetCallerName();

    SettingsEventsChannel::GetInstance().PublishAsync(
        { .source_id = CALLER.hash,
            .type = SettingsEventType::RangeChanged,
            .payload = { { SettingsPayloadType::SettingId, BrightnessSettingId() },
                { SettingsPayloadType::MinValue, static_cast<float>(BRIGHTNESS_MIN) },
                { SettingsPayloadType::MaxValue, static_cast<float>(BRIGHTNESS_MAX) },
                { SettingsPayloadType::Step, static_cast<float>(BRIGHTNESS_STEP) } } }
    );
}

void DisplayService::FollowBrightnessSensor(std::string_view sensor_id) {
    // The subscription of a sensor that is already followed stays: the publisher only asks again
    // about a sensor when the subscriptions change.
    if(sensor_id == followed_sensor_id_)
        return;

    StopFollowingBrightnessSensor();

    if(sensor_id.empty())
        return;

    EventBinding binding;
    binding.channel = EventChannelId::Sensors;
    binding.event_type = std::to_underlying(SensorEventType::DataUpdated);
    binding.payload_key = std::to_underlying(SensorPayloadType::Value);
    binding.selector_key = std::to_underlying(SensorPayloadType::SensorId);
    binding.selector_value = std::pmr::string(sensor_id, Mrm::GetExtPmr());

    // A value the bus took before the subscription went must not reach the service afterwards, so
    // the handler holds this subscription's guard and runs through it.
    auto guard = std::make_shared<DispatchGuard>(this);
    auto subscription = SubscribeEventBinding(binding, [this, guard](const EventData& value) {
        guard->Dispatch([&] {
            OnBrightnessSensorValue(value);
        });
    });

    if(subscription == nullptr) {
        const std::string id(sensor_id);
        LOG_WRN("Display brightness cannot follow sensor %s: subscribing to its values failed.", id.c_str());

        return;
    }

    brightness_sensor_guard_ = std::move(guard);
    brightness_sensor_subscription_ = std::move(subscription);
    followed_sensor_id_ = sensor_id;

    LOG_INF("Display brightness follows sensor %s.", followed_sensor_id_.c_str());
}

void DisplayService::StopFollowingBrightnessSensor() {
    // Waits for a value in progress and turns those the bus has already taken into no-ops, so none
    // reaches the service once the subscription is gone.
    if(brightness_sensor_guard_ != nullptr)
        brightness_sensor_guard_->Detach();

    brightness_sensor_subscription_.reset();
    brightness_sensor_guard_.reset();
    followed_sensor_id_.clear();
}

// A sensor sets the panel only: the stored brightness stays the one a person chose, and nothing is
// marked for writing, so a sensor that changes all the time costs no flash write. Runs on the sensor
// bus worker, in parallel with the requests on the app bus worker; lock_ keeps the state whole.
void DisplayService::OnBrightnessSensorValue(const EventData& value) {
    const auto number = ReadNumber(value);
    if(!number.has_value())
        return;

    const auto clamped = ClampBrightness(*number);

    if(clamped == GetBrightness())
        return;

    const int res = ApplyBrightness(clamped);
    if(res != 0) {
        LOG_ERR("Failed to set display brightness to %u. Error: %d.", clamped, res);
        return;
    }

    {
        ScopedMutex guard(lock_);
        panel_brightness_ = clamped;
    }

    PublishBrightnessValue();
}

bool DisplayService::IsInitialized() const {
    return is_initialized_;
}

int DisplayService::ApplyBrightness(uint8_t value) const {
    // Initialize() is what proves the device exists; without it the driver call
    // below would dereference a null device.
    if(!is_initialized_)
        return -ENODEV;

    // LVGL's flush callback reaches the same panel from the renderer thread
    // while it holds this lock, so every driver call here has to take it too.
    // Zephyr's k_mutex is recursive, so a control widget calling in from inside
    // lv_timer_handler() re-locks harmlessly.
    ScopedLvglLock lvgl_guard;

    const int res = display_set_brightness(DtDisplay::Get(), value);

    // A panel without brightness control must not make the setting fail, or the
    // control bound to it would be unusable on that board (native_sim included).
    if(res == -ENOTSUP || res == -ENOSYS) {
        LOG_WRN("Display does not support brightness control.");

        return 0;
    }

    return res;
}

int DisplayService::ApplyBlanking(bool enabled) const {
    if(!is_initialized_)
        return -ENODEV;

    ScopedLvglLock lvgl_guard;

    const int res = enabled ? display_blanking_on(DtDisplay::Get()) : display_blanking_off(DtDisplay::Get());

    if(res == -ENOTSUP || res == -ENOSYS)
        return 0;

    return res;
}

int DisplayService::SetBrightness(uint8_t value) {
    const int res = ApplyBrightness(value);
    if(res != 0) {
        LOG_ERR("Failed to set display brightness to %u. Error: %d.", value, res);
        return res;
    }

    ScopedMutex guard(lock_);
    panel_brightness_ = value;

    if(configuration_.brightness != value) {
        configuration_.brightness = value;
        is_marked_for_write_ = true;
    }

    return 0;
}

uint8_t DisplayService::GetBrightness() const {
    ScopedMutex guard(lock_);

    return panel_brightness_;
}

int DisplayService::SetBlanking(bool enabled) {
    const int res = ApplyBlanking(enabled);
    if(res != 0) {
        LOG_ERR("Failed to set display blanking. Error: %d.", res);
        return res;
    }

    ScopedMutex guard(lock_);

    if(configuration_.blanking_enabled != enabled) {
        configuration_.blanking_enabled = enabled;
        is_marked_for_write_ = true;
    }

    return 0;
}

bool DisplayService::IsBlankingEnabled() const {
    ScopedMutex guard(lock_);

    return configuration_.blanking_enabled;
}

int DisplayService::Reload() {
    const auto configuration = configuration_manager_->Get();
    if(configuration == nullptr)
        return -ENOENT;

    // The manager replaces its configuration instead of changing it, so the pointer keeps this one whole.
    const auto& snapshot = *configuration;

    ApplyBlanking(snapshot.blanking_enabled);
    ApplyBrightness(snapshot.brightness);

    {
        ScopedMutex guard(lock_);
        configuration_ = snapshot;
        panel_brightness_ = snapshot.brightness;

        // It is on flash already, and replaces a change that was waiting to be written.
        is_marked_for_write_ = false;
    }

    FollowBrightnessSensor(snapshot.brightness_sensor_id);

    PublishBrightnessValue();

    return 0;
}

int DisplayService::Persist() {
    if(!persist_task_.has_value())
        return -ENODEV;

    // Reached from the event bus worker, which must not block on flash. Requests that arrive
    // while a write is already pending coalesce into it.
    persist_task_->Reschedule(K_NO_WAIT);

    return 0;
}

WorkQueueTaskResult DisplayService::PersistWork(PersistTask* task) {
    task->service->WriteConfiguration();

    return { .reschedule = false };
}

void DisplayService::WriteConfiguration() {
    DisplayConfiguration snapshot;
    {
        ScopedMutex guard(lock_);

        // The persist request that follows a sensor's values finds nothing to write.
        if(!is_marked_for_write_)
            return;

        snapshot = configuration_;

        // Cleared with the snapshot, so a change that arrives while the file is written is not lost.
        is_marked_for_write_ = false;
    }

    // The unit's own setting (the brightness its slider sets), not a configuration update from the
    // network: it stops no logging.
    if(!configuration_manager_->UpdateOwnSetting(snapshot)) {
        LOG_ERR("Failed to persist the Display configuration.");

        // The next persist request tries again.
        ScopedMutex guard(lock_);
        is_marked_for_write_ = true;
    }
}

} // namespace eerie_leap::domain::display_domain::services
