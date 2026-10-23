#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

#include <zephyr/kernel.h>

#include "subsys/threading/work_queue_task.h"
#include "subsys/threading/work_queue_thread.h"

#include "domain/ui_domain/preview/ui_preview_session.h"
#include "domain/ui_preview_domain/i_ui_preview_host.h"

namespace eerie_leap::domain::ui_preview_domain {

using eerie_leap::domain::ui_domain::preview::UiPreviewError;
using eerie_leap::domain::ui_domain::preview::UiPreviewSession;
using eerie_leap::subsys::threading::WorkQueueTask;
using eerie_leap::subsys::threading::WorkQueueTaskResult;
using eerie_leap::subsys::threading::WorkQueueThread;

/**
 * @brief The UI preview: shows a configuration a client stages on the screens, without storing it, and puts
 *        the stored one back when the session ends.
 *
 * A preview stores nothing, so it stops no logging: the staged bytes are checked as an upload is
 * (IUiPreviewHost::ParseConfiguration()), and shown by the path a store's rebuild takes, with no stored handler
 * called. One session exists at a time; its token is the client's lease, renewed by every request that carries
 * it and enforced here by a delayable task. It ends, and the stored screens come back, when the client says so
 * (END), when the lease runs out, and when the local link drops (OnLinkDropped()); when a configuration is
 * stored (OnConfigurationStored()) it ends without a revert, since the store's own rebuild shows what it stored.
 *
 * The requests come from the MCUmgr work queue, whose handler calls the blocking methods below: they allocate
 * and throw nothing on that stack, and hand the work to the queue it belongs to as a job while the handler
 * waits on a semaphore: the decoding to the configuration work queue (its stack decodes an upload), the screens
 * to the UI render work queue. The lease task, the end task and the jobs of the render work queue run one after
 * the other there, which is what orders a session's end against what it is doing.
 */
class UiPreviewService {
public:
    static constexpr size_t MAX_REASON_LENGTH = 191;

    /** @brief What BEGIN answers. */
    struct BeginInfo {
        uint32_t token = 0;
        uint32_t lease_ms = 0;
        uint32_t rev = 0;
        uint32_t stored_crc = 0;
        /// The most a PATCH may carry; 0 while the unit takes none (FW-2).
        uint32_t max_patch = 0;
        uint32_t max_stage = 0;
    };

    struct StageRequest {
        uint32_t token = 0;
        uint32_t offset = 0;
        std::span<const uint8_t> data;
        std::optional<uint32_t> length;
        std::optional<uint32_t> crc;
        std::optional<uint32_t> rev;
    };

    /** @brief What STAGE answers; @p rev and @p applied are there once the configuration was applied. */
    struct StageInfo {
        uint32_t received = 0;
        std::optional<uint32_t> rev;
        std::optional<UiPreviewApplied> applied;
        /// For STALE, the revision applied.
        uint32_t current_rev = 0;
    };

    struct Status {
        bool is_active = false;
        uint32_t rev = 0;
        uint32_t lease_left_ms = 0;
        uint32_t stored_crc = 0;
        std::optional<uint32_t> group;
    };

private:
    enum class JobKind : uint8_t {
        BEGIN,
        PARSE,
        APPLY,
        SHOW,
        HIGHLIGHT,
        END,
    };

    struct Job {
        JobKind kind = JobKind::BEGIN;
        std::optional<uint32_t> token;
        uint32_t required_token = 0;
        bool is_pinned = true;
        UiPreviewTarget target;
        std::optional<uint32_t> screen;
        std::optional<uint32_t> widget;

        UiPreviewError result = UiPreviewError::UNKNOWN;
        BeginInfo begin;
        UiPreviewShown shown;
        UiPreviewApplied applied;
        bool is_reverted = false;
        uint32_t rev = 0;
    };

    static constexpr uint8_t END_LINK_DROPPED = 1U << 0;
    static constexpr uint8_t END_STORED = 1U << 1;
    // Waiting for a staged configuration to finish being applied before a session may end.
    static constexpr int END_RETRY_MS = 20;

    IUiPreviewHost& host_;
    WorkQueueThread& render_queue_;
    WorkQueueThread& config_queue_;
    UiPreviewSession session_;

    std::optional<WorkQueueTask<UiPreviewService>> render_job_task_;
    std::optional<WorkQueueTask<UiPreviewService>> config_job_task_;
    std::optional<WorkQueueTask<UiPreviewService>> lease_task_;
    std::optional<WorkQueueTask<UiPreviewService>> end_task_;

    // One job at a time, whichever queue it is on; the handler that submitted it waits on job_done_.
    k_mutex job_lock_{};
    k_sem job_done_{};
    Job* job_ = nullptr;

    std::atomic<uint8_t> pending_end_ = 0;
    // The group shown at BEGIN (render work queue only), where a session that only navigated leaves the gauge.
    std::optional<uint32_t> begin_group_;

    std::shared_ptr<UiConfiguration> parsed_;
    std::array<char, MAX_REASON_LENGTH + 1> reason_{};

    UiPreviewError RunJob(std::optional<WorkQueueTask<UiPreviewService>>& task, Job& job);
    WorkQueueTaskResult ProcessJob();
    UiPreviewError ExecuteJob(Job& job);
    UiPreviewError ExecuteBegin(Job& job);
    UiPreviewError ExecuteParse();
    UiPreviewError ExecuteApply(Job& job);

    WorkQueueTaskResult ProcessLease();
    WorkQueueTaskResult ProcessEnd();

    /**
     * @brief Ends the session on the render work queue.
     * @return Whether the stored screens had to come back (null while a staged configuration is being applied: the
     *         session stays, and the end is asked again).
     */
    std::optional<bool> EndSession(bool is_revert);
    /** @brief Shows the stored screens again after a show that failed, or half did. Never throws. */
    void RestoreStoredScreens();
    /** @brief Puts navigation back on the group shown at BEGIN, with no overlay. */
    void RestoreNavigation();

public:
    /**
     * @param host The UI the preview shows on; outlives the service.
     * @param render_queue Rebuilds the screens, runs the lease and the ends.
     * @param config_queue Decodes the staged configuration, as an upload is.
     */
    UiPreviewService(IUiPreviewHost& host, WorkQueueThread& render_queue, WorkQueueThread& config_queue);
    ~UiPreviewService();

    UiPreviewService(const UiPreviewService&) = delete;
    UiPreviewService& operator=(const UiPreviewService&) = delete;
    UiPreviewService(UiPreviewService&&) = delete;
    UiPreviewService& operator=(UiPreviewService&&) = delete;

    /** @brief Creates the tasks; both queues must run. */
    bool Initialize();

    /** @brief Opens a session, or renews or repeats one (see UiPreviewSession::Begin()). */
    UiPreviewError Begin(std::optional<uint32_t> token, bool is_pinned, BeginInfo& info);

    /** @brief Takes a chunk of the staged configuration; the last one shows it. */
    UiPreviewError Stage(const StageRequest& request, StageInfo& info);

    UiPreviewError Show(uint32_t token, const UiPreviewTarget& target, UiPreviewShown& shown);
    UiPreviewError Highlight(uint32_t token, std::optional<uint32_t> screen, std::optional<uint32_t> widget);

    /** @brief Ends the session and puts the stored screens back; @p is_reverted says whether a staged one was shown. */
    UiPreviewError End(uint32_t token, bool& is_reverted);

    /** @brief Needs no token, and no queue. */
    [[nodiscard]] Status GetStatus() const;

    /** @brief Why the last STAGE was INVALID: the validator's message. Valid until the next STAGE. */
    [[nodiscard]] std::string_view GetReason() const;

    /** @brief The local link dropped (BLE disconnect): the session ends, and the stored screens come back. Any thread. */
    void OnLinkDropped();

    /** @brief A configuration was stored: the session ends and shows nothing more. Any thread, on the storing thread's cheap path. */
    void OnConfigurationStored();

    [[nodiscard]] bool IsActive() const;
    [[nodiscard]] const UiPreviewSession& GetSession() const {
        return session_;
    }
};

} // namespace eerie_leap::domain::ui_preview_domain
