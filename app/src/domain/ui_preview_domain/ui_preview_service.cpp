#include <zephyr/kernel.h>

#ifdef CONFIG_EERIE_LEAP_UI_PREVIEW

    #include <algorithm>
    #include <cerrno>
    #include <cstring>
    #include <exception>
    #include <new>
    #include <utility>

    #include <zephyr/logging/log.h>
    #include <zephyr/sys/crc.h>

    #include "subsys/random/rng.h"
    #include "subsys/threading/scoped_mutex.h"
    #include "utilities/memory/memory_resource_manager.h"

    #include "ui_preview_service.h"

LOG_MODULE_REGISTER(ui_preview_service, LOG_LEVEL_INF);

namespace eerie_leap::domain::ui_preview_domain {

using eerie_leap::subsys::random::Rng;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::utilities::memory::Mrm;

namespace {

constexpr int64_t LEASE_MS = CONFIG_EERIE_LEAP_UI_PREVIEW_LEASE_MS;
constexpr uint32_t MAX_STAGE_SIZE = CONFIG_EERIE_LEAP_UI_PREVIEW_MAX_STAGE_SIZE;

uint32_t NewToken() {
    uint32_t token = 0;
    while(token == 0)
        token = Rng::Get<uint32_t>();

    return token;
}

uint32_t Clamp32(int64_t value) {
    return static_cast<uint32_t>(std::clamp<int64_t>(value, 0, UINT32_MAX));
}

} // namespace

UiPreviewService::UiPreviewService(IUiPreviewHost& host, WorkQueueThread& render_queue, WorkQueueThread& config_queue) :
    host_(host),
    render_queue_(render_queue),
    config_queue_(config_queue),
    session_(LEASE_MS, Mrm::GetExtPmr()) {

    k_mutex_init(&job_lock_);
    k_sem_init(&job_done_, 0, 1);
}

UiPreviewService::~UiPreviewService() {
    // Nothing waits on a task once it is gone, and a session in the middle of its life has no one to put the stored
    // screens back for: the controller does that when it is destroyed.
    for(auto* task : { &render_job_task_, &config_job_task_, &lease_task_, &end_task_ }) {
        if(task->has_value())
            (*task)->Cancel();
    }
}

bool UiPreviewService::Initialize() {
    if(render_job_task_.has_value())
        return true;

    try {
        render_job_task_ = render_queue_.CreateTask(
            [](UiPreviewService* service) {
                return service->ProcessJob();
            },
            this
        );
        config_job_task_ = config_queue_.CreateTask(
            [](UiPreviewService* service) {
                return service->ProcessJob();
            },
            this
        );
        lease_task_ = render_queue_.CreateTask(
            [](UiPreviewService* service) {
                return service->ProcessLease();
            },
            this
        );
        end_task_ = render_queue_.CreateTask(
            [](UiPreviewService* service) {
                return service->ProcessEnd();
            },
            this
        );
    } catch(const std::exception& e) {
        LOG_ERR("Failed to create the UI preview tasks: %s", e.what());
        render_job_task_.reset();
        config_job_task_.reset();
        lease_task_.reset();
        end_task_.reset();

        return false;
    }

    return true;
}

UiPreviewError UiPreviewService::RunJob(std::optional<WorkQueueTask<UiPreviewService>>& task, Job& job) {
    // Initialize() has not created the tasks, or failed to.
    if(!task.has_value())
        return UiPreviewError::UNKNOWN;

    const ScopedMutex serialize(job_lock_);

    job_ = &job;
    // A queue that took nothing runs nothing, and there would be no one to give the semaphore.
    if(task->Schedule() <= 0) {
        job_ = nullptr;
        LOG_ERR("Failed to queue a UI preview job.");

        return UiPreviewError::UNKNOWN;
    }

    k_sem_take(&job_done_, K_FOREVER);
    job_ = nullptr;

    return job.result;
}

WorkQueueTaskResult UiPreviewService::ProcessJob() {
    Job* job = job_;
    if(job == nullptr)
        return {};

    try {
        job->result = ExecuteJob(*job);
    } catch(const std::bad_alloc&) {
        job->result = UiPreviewError::NO_MEMORY;
    } catch(const std::exception& e) {
        LOG_ERR("UI preview job failed: %s", e.what());
        job->result = UiPreviewError::UNKNOWN;
    }

    k_sem_give(&job_done_);

    return {};
}

UiPreviewError UiPreviewService::ExecuteJob(Job& job) {
    switch(job.kind) {
        case JobKind::BEGIN:
            return ExecuteBegin(job);

        case JobKind::PARSE:
            return ExecuteParse();

        case JobKind::APPLY:
            return ExecuteApply(job);

        case JobKind::SHOW: {
            if(const auto error = session_.Touch(job.required_token, k_uptime_get()); error != UiPreviewError::OK)
                return error;

            const auto error = host_.ShowTarget(job.target, job.shown);
            if(error == UiPreviewError::OK)
                session_.MarkNavigated();

            return error;
        }

        case JobKind::HIGHLIGHT: {
            if(const auto error = session_.Touch(job.required_token, k_uptime_get()); error != UiPreviewError::OK)
                return error;

            return host_.Highlight(job.screen, job.widget);
        }

        case JobKind::END: {
            if(const auto error = session_.Touch(job.required_token, k_uptime_get()); error != UiPreviewError::OK)
                return error;

            const auto is_reverted = EndSession(true);

            // The client's own request cannot find a configuration being applied: it is the one applying it.
            if(!is_reverted.has_value())
                return UiPreviewError::BUSY;

            job.is_reverted = *is_reverted;

            return UiPreviewError::OK;
        }
    }

    return UiPreviewError::UNKNOWN;
}

UiPreviewError UiPreviewService::ExecuteBegin(Job& job) {
    const int64_t now_ms = k_uptime_get();
    const auto begun = session_.Begin(job.token, job.is_pinned, NewToken(), now_ms);
    if(begun.error != UiPreviewError::OK)
        return begun.error;

    if(begun.is_created) {
        // Allocated here, not by a chunk, so a transfer never meets a full heap halfway.
        if(const auto error = session_.AllocateBuffer(MAX_STAGE_SIZE); error != UiPreviewError::OK) {
            session_.End();

            return error;
        }

        begin_group_ = host_.GetActiveGroupId();
        host_.SetNavigationPinned(job.is_pinned);
        if(lease_task_.has_value())
            lease_task_->Reschedule(K_MSEC(LEASE_MS));

        LOG_INF("UI preview opened.");
    }

    job.begin = { .token = begun.token,
        .lease_ms = static_cast<uint32_t>(LEASE_MS),
        .rev = session_.GetRev(),
        .stored_crc = host_.GetStoredCrc(),
        .max_patch = 0,
        .max_stage = MAX_STAGE_SIZE };

    return UiPreviewError::OK;
}

UiPreviewError UiPreviewService::ExecuteParse() {
    const auto staged = session_.Staged();
    if(staged.empty())
        return UiPreviewError::NO_SESSION;

    if(crc32_ieee(staged.data(), staged.size()) != session_.GetStagedCrc())
        return UiPreviewError::CRC_MISMATCH;

    reason_.front() = '\0';
    parsed_ = host_.ParseConfiguration(staged, reason_);
    if(parsed_ == nullptr) {
        if(reason_.front() == '\0')
            std::strncpy(reason_.data(), "The configuration was refused.", reason_.size() - 1);

        return UiPreviewError::INVALID;
    }

    return UiPreviewError::OK;
}

UiPreviewError UiPreviewService::ExecuteApply(Job& job) {
    auto configuration = std::move(parsed_);
    parsed_.reset();

    if(!session_.IsActive() || configuration == nullptr)
        return UiPreviewError::NO_SESSION;

    // Marked before the screens go: a show that fails half way has dropped the stored ones all the same, and
    // whatever ends the session then has to put them back.
    session_.MarkScreensReplaced();

    int res = 0;
    try {
        res = host_.ShowConfiguration(std::move(configuration), job.applied);
    } catch(...) {
        RestoreStoredScreens();

        throw;
    }

    // An empty configuration has no screen to show and is still a configuration: the screens it replaces are gone.
    if(res != 0 && res != -ENOENT) {
        LOG_ERR("Failed to show the staged UI configuration. Error: %d.", res);
        RestoreStoredScreens();

        return UiPreviewError::UNKNOWN;
    }

    job.rev = session_.CommitStage();

    return UiPreviewError::OK;
}

void UiPreviewService::RestoreStoredScreens() {
    try {
        const int res = host_.ShowStoredConfiguration();
        if(res != 0 && res != -ENOENT) {
            LOG_ERR("Failed to show the stored UI configuration again. Error: %d.", res);

            return;
        }

        session_.MarkScreensRestored();
    } catch(const std::exception& e) {
        LOG_ERR("Failed to show the stored UI configuration again. %s", e.what());
    } catch(...) {
        LOG_ERR("Failed to show the stored UI configuration again.");
    }
}

void UiPreviewService::RestoreNavigation() {
    if(!begin_group_.has_value())
        return;

    UiPreviewShown shown;
    // The group may be gone from the stored configuration: then it stays where it is.
    host_.ShowTarget({ .is_overlay = false, .id = *begin_group_ }, shown);
}

std::optional<bool> UiPreviewService::EndSession(bool is_revert) {
    const auto ended = session_.End();
    if(!ended.is_ended)
        return std::nullopt;

    // Not synchronously: this can be the lease task itself. Only once the session is over, as a deferred end leaves the
    // lease to look again.
    if(lease_task_.has_value())
        k_work_cancel_delayable(&lease_task_->work);

    // Whatever asked for an end is answered by this one.
    pending_end_ = 0;

    host_.SetNavigationPinned(false);
    host_.Highlight(std::nullopt, std::nullopt);

    bool is_reverted = false;
    if(is_revert) {
        if(ended.had_replaced_screens) {
            RestoreStoredScreens();
            is_reverted = true;
        }

        // The stored screens start on their start group, and a SHOW alone left the gauge elsewhere.
        if(ended.had_replaced_screens || ended.had_navigated)
            RestoreNavigation();
    }

    return is_reverted;
}

WorkQueueTaskResult UiPreviewService::ProcessLease() {
    if(!session_.IsActive())
        return {};

    const int64_t now_ms = k_uptime_get();
    if(session_.IsLeaseExpired(now_ms) && !session_.IsApplying()) {
        LOG_INF("UI preview lease ran out.");
        if(EndSession(true).has_value())
            return {};
    }

    // Requests renewed it, or a configuration is being applied: look again when it would end.
    const int64_t left_ms = session_.GetLeaseLeftMs(now_ms);

    return { .reschedule = true, .delay = K_MSEC(std::max<int64_t>(left_ms, END_RETRY_MS)) };
}

WorkQueueTaskResult UiPreviewService::ProcessEnd() {
    if(!session_.IsActive()) {
        pending_end_ = 0;

        return {};
    }

    if(session_.IsApplying())
        return { .reschedule = true, .delay = K_MSEC(END_RETRY_MS) };

    const uint8_t reasons = pending_end_.load();
    if(reasons == 0)
        return {};

    // A store's own rebuild shows what it stored; the link going away leaves the stored screens to come back.
    const bool is_stored = (reasons & END_STORED) != 0;
    if(!EndSession(!is_stored).has_value())
        return { .reschedule = true, .delay = K_MSEC(END_RETRY_MS) };

    LOG_INF("UI preview ended: %s.", is_stored ? "a configuration was stored" : "the link dropped");

    return {};
}

UiPreviewError UiPreviewService::Begin(std::optional<uint32_t> token, bool is_pinned, BeginInfo& info) {
    Job job{ .kind = JobKind::BEGIN, .token = token, .is_pinned = is_pinned };
    const auto error = RunJob(render_job_task_, job);
    if(error == UiPreviewError::OK)
        info = job.begin;

    return error;
}

UiPreviewError UiPreviewService::Stage(const StageRequest& request, StageInfo& info) {
    const auto staged = session_.Stage(
        request.token, request.offset, request.data, request.length, request.crc, request.rev, k_uptime_get()
    );
    info.received = staged.received;
    info.current_rev = staged.rev;

    if(staged.error != UiPreviewError::OK)
        return staged.error;

    if(!staged.is_complete)
        return UiPreviewError::OK;

    // The last chunk: decode and check what was staged, as an upload is, then show it. The session is applying
    // from the chunk on, so no end takes the buffer meanwhile.
    Job parse{ .kind = JobKind::PARSE, .required_token = request.token };
    auto error = RunJob(config_job_task_, parse);
    if(error == UiPreviewError::OK) {
        Job apply{ .kind = JobKind::APPLY, .required_token = request.token };
        error = RunJob(render_job_task_, apply);
        if(error == UiPreviewError::OK) {
            info.rev = apply.rev;
            info.applied = apply.applied;
        }
    }

    parsed_.reset();

    // The transfer is over, whatever came of it.
    session_.DropStage();
    session_.FinishApply();

    // The lease runs from the end of the apply, which can take longer than the lease itself.
    session_.Touch(request.token, k_uptime_get());

    return error;
}

UiPreviewError UiPreviewService::Show(uint32_t token, const UiPreviewTarget& target, UiPreviewShown& shown) {
    Job job{ .kind = JobKind::SHOW, .required_token = token, .target = target };
    const auto error = RunJob(render_job_task_, job);
    if(error == UiPreviewError::OK)
        shown = job.shown;

    return error;
}

UiPreviewError
UiPreviewService::Highlight(uint32_t token, std::optional<uint32_t> screen, std::optional<uint32_t> widget) {
    Job job{ .kind = JobKind::HIGHLIGHT, .required_token = token, .screen = screen, .widget = widget };

    return RunJob(render_job_task_, job);
}

UiPreviewError UiPreviewService::End(uint32_t token, bool& is_reverted) {
    Job job{ .kind = JobKind::END, .required_token = token };
    const auto error = RunJob(render_job_task_, job);
    if(error == UiPreviewError::OK)
        is_reverted = job.is_reverted;

    return error;
}

UiPreviewService::Status UiPreviewService::GetStatus() const {
    const auto snapshot = session_.GetSnapshot(k_uptime_get());

    Status status{
        .is_active = snapshot.is_active, .stored_crc = host_.GetStoredCrc(), .group = host_.GetActiveGroupId()
    };
    if(snapshot.is_active) {
        status.rev = snapshot.rev;
        status.lease_left_ms = Clamp32(snapshot.lease_left_ms);
    }

    return status;
}

std::string_view UiPreviewService::GetReason() const {
    return { reason_.data() };
}

void UiPreviewService::OnLinkDropped() {
    if(!session_.IsActive() || !end_task_.has_value())
        return;

    pending_end_ |= END_LINK_DROPPED;
    end_task_->Reschedule(K_NO_WAIT);
}

void UiPreviewService::OnConfigurationStored() {
    if(!session_.IsActive() || !end_task_.has_value())
        return;

    pending_end_ |= END_STORED;
    end_task_->Reschedule(K_NO_WAIT);
}

bool UiPreviewService::IsActive() const {
    return session_.IsActive();
}

} // namespace eerie_leap::domain::ui_preview_domain

#endif // CONFIG_EERIE_LEAP_UI_PREVIEW
