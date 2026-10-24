#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/threading/work_queue_task.h"
#include "subsys/threading/work_queue_thread.h"

#include "domain/ui_domain/preview/ui_patch.h"
#include "domain/ui_domain/preview/ui_preview_session.h"
#include "domain/ui_preview_domain/i_ui_preview_host.h"

namespace eerie_leap::domain::ui_preview_domain {

using eerie_leap::domain::ui_domain::preview::PatchEffect;
using eerie_leap::domain::ui_domain::preview::PatchOp;
using eerie_leap::domain::ui_domain::preview::PatchOpKind;
using eerie_leap::domain::ui_domain::preview::PatchPlan;
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

    /// The most operations one PATCH takes.
    static constexpr size_t MAX_PATCH_OPS = 16;
    /// The most bytes the CBOR body of one PATCH request may take, `tok`, `rev` and `ops` with their keys included:
    /// one SMP packet less its 8-byte header.
#ifdef CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE
    static constexpr size_t MAX_PATCH_SIZE = CONFIG_MCUMGR_TRANSPORT_NETBUF_SIZE - 8;
#else
    static constexpr size_t MAX_PATCH_SIZE = 1024 - 8;
#endif

    /** @brief What BEGIN answers. */
    struct BeginInfo {
        uint32_t token = 0;
        uint32_t lease_ms = 0;
        uint32_t rev = 0;
        uint32_t stored_crc = 0;
        /// The most bytes a PATCH request may take.
        uint32_t max_patch = 0;
        /// The most operations a PATCH takes.
        uint32_t max_ops = 0;
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

    /// One operation of a PATCH as the request carries it; the bytes are the widget or screen record, in the request.
    struct PatchOpRef {
        PatchOpKind kind = PatchOpKind::PutWidget;
        uint32_t screen_id = 0;
        uint32_t widget_id = 0;
        std::span<const uint8_t> bytes;
    };

    struct PatchRequest {
        uint32_t token = 0;
        uint32_t rev = 0;
        std::span<const PatchOpRef> ops;
        /// The size of the request's CBOR body, as it came.
        size_t body_size = 0;
    };

    /// What PATCH answers.
    struct PatchInfo {
        uint32_t rev = 0;
        std::array<PatchEffect, MAX_PATCH_OPS> effects{};
        size_t effect_count = 0;
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
        PATCH_PREPARE,
        PATCH_APPLY,
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
        const PatchRequest* patch = nullptr;
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
    // The configuration the screens show, as the last stage or patch left it: what a patch applies onto. Null
    // before anything was shown and after the stored screens came back. Touched by jobs and ends, which never
    // overlap an apply.
    std::shared_ptr<UiConfiguration> staged_;
    // A patch in the making, between its two jobs.
    std::shared_ptr<UiConfiguration> patched_;
    std::vector<PatchOp> patch_ops_;
    PatchPlan patch_plan_;
    std::array<char, MAX_REASON_LENGTH + 1> reason_{};

    UiPreviewError RunJob(std::optional<WorkQueueTask<UiPreviewService>>& task, Job& job);
    WorkQueueTaskResult ProcessJob();
    UiPreviewError ExecuteJob(Job& job);
    UiPreviewError ExecuteBegin(Job& job);
    UiPreviewError ExecuteParse();
    UiPreviewError ExecuteApply(Job& job);
    UiPreviewError ExecutePatchPrepare(Job& job);
    UiPreviewError ExecutePatchApply(Job& job);
    void SetReason(std::string_view text);

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

    /**
     * @brief Applies the operations in order onto the configuration the screens show, all or nothing, and shows the
     *        result. Decoded on the configuration work queue and checked as an upload is; shown on the render one.
     * @return BASE_MISMATCH when nothing was staged, or an operation names a screen or widget the staged
     *         configuration lacks (the client stages the whole configuration again); INVALID with the reason for
     *         records the validator refuses; TOO_LARGE for more operations or bytes than a request may carry; STALE
     *         with the applied revision in @p info; BUSY while a stage or patch is being applied.
     */
    UiPreviewError Patch(const PatchRequest& request, PatchInfo& info);

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
