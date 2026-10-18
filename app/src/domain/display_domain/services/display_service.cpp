#include <algorithm>
#include <cerrno>
#include <cmath>
#include <utility>

#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <zephyr/logging/log.h>

#include "subsys/device_tree/dt_display.h"
#include "subsys/threading/scoped_mutex.h"
#include "utilities/reflection/caller_name.h"
#include "utilities/string/string_helpers.h"

#include "domain/settings_domain/models/setting_id.h"
#include "domain/ui_domain/lvgl_lock.h"

#include "display_service.h"

namespace eerie_leap::domain::display_domain::services {

using eerie_leap::domain::settings_domain::models::SettingId;
using eerie_leap::domain::ui_domain::ScopedLvglLock;
using eerie_leap::subsys::device_tree::DtDisplay;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::utilities::reflection::GetCallerName;
using eerie_leap::utilities::string::StringHelpers;

using namespace eerie_leap::domain::settings_domain::event_bus;

LOG_MODULE_REGISTER(display_service_logger);

namespace {

uint32_t BrightnessSettingId() {
    static const uint32_t id = StringHelpers::GetHash(SettingId::DISPLAY_BRIGHTNESS);

    return id;
}

bool IsBrightnessSetting(const SettingsEventsChannel::EventMessage& event) {
    auto it = event.payload.find(SettingsPayloadType::SettingId);
    if(it == event.payload.end())
        return false;

    const auto* id = std::get_if<uint32_t>(&it->second);

    return id != nullptr && *id == BrightnessSettingId();
}

// A request that names no setting is addressed to every owner.
bool IsBrightnessStateRequest(const SettingsEventsChannel::EventMessage& event) {
    return event.payload.find(SettingsPayloadType::SettingId) == event.payload.end() || IsBrightnessSetting(event);
}

} // namespace

DisplayService::DisplayService(
    std::shared_ptr<DisplayConfigurationManager> configuration_manager,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread
) :
    configuration_manager_(std::move(configuration_manager)),
    config_work_queue_thread_(std::move(config_work_queue_thread)) {

    k_mutex_init(&lock_);
}

DisplayService::~DisplayService() {
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
    }

    if(config_work_queue_thread_ != nullptr) {
        auto task = std::make_unique<PersistTask>();
        task->service = this;

        persist_task_ = config_work_queue_thread_->CreateTask(PersistWork, std::move(task));
    }

    ApplyBlanking(snapshot.blanking_enabled);
    ApplyBrightness(snapshot.brightness);

    SubscribeSettings();

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
    auto it = event.payload.find(SettingsPayloadType::Value);
    if(it == event.payload.end())
        return;

    double requested = 0;
    if(const auto* int_value = std::get_if<int>(&it->second))
        requested = *int_value;
    else if(const auto* float_value = std::get_if<float>(&it->second))
        requested = *float_value;
    else
        return;

    const auto clamped = static_cast<uint8_t>(std::lround(std::clamp(requested, brightness_min_, brightness_max_)));

    // Only a value that actually moved is a fact worth waking every subscriber - and worth
    // re-arming the persistence debounce - for.
    if(clamped == GetBrightness())
        return;

    if(SetBrightness(clamped) != 0)
        return;

    PublishBrightnessValue();
}

void DisplayService::PublishBrightnessValue() const {
    static constexpr auto caller = GetCallerName();

    SettingsEventsChannel::GetInstance().PublishAsync(
        { .source_id = caller.hash,
            .type = SettingsEventType::Changed,
            .payload = { { SettingsPayloadType::SettingId, BrightnessSettingId() },
                { SettingsPayloadType::Value, static_cast<int>(GetBrightness()) } } }
    );
}

void DisplayService::PublishBrightnessRange() const {
    static constexpr auto caller = GetCallerName();

    SettingsEventsChannel::GetInstance().PublishAsync(
        { .source_id = caller.hash,
            .type = SettingsEventType::RangeChanged,
            .payload = { { SettingsPayloadType::SettingId, BrightnessSettingId() },
                { SettingsPayloadType::MinValue, static_cast<float>(brightness_min_) },
                { SettingsPayloadType::MaxValue, static_cast<float>(brightness_max_) },
                { SettingsPayloadType::Step, static_cast<float>(brightness_step_) } } }
    );
}

bool DisplayService::IsInitialized() const {
    return is_initialized_;
}

int DisplayService::ApplyBrightness(uint8_t value) {
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

int DisplayService::ApplyBlanking(bool enabled) {
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
    configuration_.brightness = value;

    return 0;
}

uint8_t DisplayService::GetBrightness() const {
    ScopedMutex guard(lock_);

    return configuration_.brightness;
}

int DisplayService::SetBlanking(bool enabled) {
    const int res = ApplyBlanking(enabled);
    if(res != 0) {
        LOG_ERR("Failed to set display blanking. Error: %d.", res);
        return res;
    }

    ScopedMutex guard(lock_);
    configuration_.blanking_enabled = enabled;

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

    DisplayConfiguration snapshot = *configuration;

    ApplyBlanking(snapshot.blanking_enabled);
    ApplyBrightness(snapshot.brightness);

    {
        ScopedMutex guard(lock_);
        configuration_ = snapshot;
    }

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
        snapshot = configuration_;
    }

    // The unit's own setting (the brightness its slider sets), not a configuration update from the
    // network: it stops no logging.
    if(!configuration_manager_->UpdateOwnSetting(snapshot))
        LOG_ERR("Failed to persist the Display configuration.");
}

} // namespace eerie_leap::domain::display_domain::services
