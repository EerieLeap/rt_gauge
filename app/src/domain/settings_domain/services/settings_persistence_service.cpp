#include <algorithm>
#include <cerrno>
#include <utility>

#include <zephyr/logging/log.h>

#include "subsys/threading/scoped_mutex.h"
#include "utilities/reflection/caller_name.h"

#include "domain/settings_domain/event_bus/settings_events_channel.h"

#include "settings_persistence_service.h"

namespace eerie_leap::domain::settings_domain::services {

using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::utilities::reflection::GetCallerName;

using namespace eerie_leap::domain::settings_domain::event_bus;

LOG_MODULE_REGISTER(settings_persistence_service_logger);

SettingsPersistenceService::SettingsPersistenceService(std::shared_ptr<WorkQueueThread> work_queue_thread) :
    work_queue_thread_(std::move(work_queue_thread)) {

    k_mutex_init(&lock_);
}

SettingsPersistenceService::~SettingsPersistenceService() {
    changed_subscription_.reset();

    if(persist_task_.has_value())
        persist_task_->Cancel();
}

int SettingsPersistenceService::Initialize() {
    if(work_queue_thread_ == nullptr)
        return -ENODEV;

    auto task = std::make_unique<PersistTask>();
    task->service = this;

    persist_task_ = work_queue_thread_->CreateTask(PersistWork, std::move(task));

    changed_subscription_ = CreateScopedSubscription(
        SettingsEventsChannel::GetInstance(), SettingsEventType::Changed,
        [this](const SettingsEventsChannel::EventMessage& event) {
            const auto* it = event.payload.find(SettingsPayloadType::SettingId);
            if(it == event.payload.end())
                return;

            const auto* setting_id = std::get_if<uint32_t>(&it->second);
            if(setting_id == nullptr)
                return;

            {
                ScopedMutex guard(lock_);

                if(std::ranges::find(pending_setting_ids_, *setting_id) == pending_setting_ids_.end())
                    pending_setting_ids_.push_back(*setting_id);
            }

            // Every change pushes the deadline out, so a drag reaches flash once.
            persist_task_->Reschedule(PERSIST_DEBOUNCE);
        }
    );

    if(changed_subscription_ == nullptr) {
        LOG_ERR("Failed to subscribe to setting changes.");
        return -EIO;
    }

    return 0;
}

WorkQueueTaskResult SettingsPersistenceService::PersistWork(PersistTask* task) {
    task->service->PublishPendingPersistRequests();

    return { .reschedule = false };
}

void SettingsPersistenceService::PublishPendingPersistRequests() {
    static constexpr auto CALLER = GetCallerName();

    std::vector<uint32_t> setting_ids;
    {
        ScopedMutex guard(lock_);
        setting_ids.swap(pending_setting_ids_);
    }

    for(auto setting_id : setting_ids) {
        SettingsEventsChannel::GetInstance().PublishAsync(
            { .source_id = CALLER.hash,
                .type = SettingsEventType::PersistRequested,
                .payload = { { SettingsPayloadType::SettingId, setting_id } } }
        );
    }
}

} // namespace eerie_leap::domain::settings_domain::services
