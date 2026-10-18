#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/event_bus/event_channel.h"
#include "subsys/threading/work_queue_task.h"
#include "subsys/threading/work_queue_task_result.h"
#include "subsys/threading/work_queue_thread.h"

namespace eerie_leap::domain::settings_domain::services {

using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::threading::WorkQueueTask;
using eerie_leap::subsys::threading::WorkQueueTaskResult;
using eerie_leap::subsys::threading::WorkQueueThread;

// Turns a burst of setting changes into one persist request per setting, once the user has
// stopped moving things. Owners subscribe to PersistRequested and write their own configuration;
// nothing here knows where a setting is actually stored.
class SettingsPersistenceService {
private:
    struct PersistTask {
        SettingsPersistenceService* service = nullptr;
    };

    static constexpr k_timeout_t persist_debounce_ = K_MSEC(1000);

    std::shared_ptr<WorkQueueThread> work_queue_thread_;

    std::vector<uint32_t> pending_setting_ids_;
    mutable k_mutex lock_{};

    AnySubscription changed_subscription_;
    std::optional<WorkQueueTask<PersistTask>> persist_task_;

    static WorkQueueTaskResult PersistWork(PersistTask* task);
    void PublishPendingPersistRequests();

public:
    explicit SettingsPersistenceService(std::shared_ptr<WorkQueueThread> work_queue_thread);
    ~SettingsPersistenceService();

    SettingsPersistenceService(const SettingsPersistenceService&) = delete;
    SettingsPersistenceService& operator=(const SettingsPersistenceService&) = delete;

    int Initialize();
};

} // namespace eerie_leap::domain::settings_domain::services
