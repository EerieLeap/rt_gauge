#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

#include <zephyr/kernel.h>

namespace eerie_leap::domain::ui_domain::preview {

/** @brief The `ui` SMP group's errors, as the group answers them in the SMP v2 `err` map. */
enum class UiPreviewError : uint16_t {
    OK = 0,
    UNKNOWN = 1,
    NO_SESSION = 2,    ///< No preview is open.
    BAD_TOKEN = 3,     ///< The token is not the open session's.
    BUSY = 4,          ///< Another client has a session open.
    STALE = 5,         ///< The revision is not past the applied one (the response carries it).
    BASE_MISMATCH = 6, ///< A patch's base is not the staged configuration (FW-2).
    TOO_LARGE = 7,     ///< The configuration exceeds the stage buffer, or the data exceeds the length.
    BAD_OFFSET = 8,    ///< Not the next offset (the response carries it).
    CRC_MISMATCH = 9,
    INVALID = 10,   ///< The configuration is refused, as an upload would be; `msg` says why.
    NOT_FOUND = 11, ///< The group, overlay, screen or widget is not in the configuration shown.
    NO_MEMORY = 12, ///< The stage buffer could not be allocated.
};

/**
 * @brief The state of one UI preview: its token, lease, revision and the chunks of a staged
 *        configuration. Pure bookkeeping, with no queue, display or storage of its own.
 *
 * At most one session exists. A request that carries the token renews the lease, which the
 * owner (UiPreviewService) enforces: this class only answers whether it ran out. Everything is
 * guarded by one mutex, so the MCUmgr handlers and the work queues may call it. Nothing allocates
 * but AllocateBuffer(), which the owner calls from a work queue: the calls a handler makes
 * (Touch(), Stage(), GetSnapshot()) allocate nothing and throw nothing.
 *
 * The staged configuration is a whole CborUiConfig in the buffer, received in order like a
 * `config` write. A later PATCH (FW-2) applies to the model that staged configuration made, on the
 * revisions this class counts.
 */
class UiPreviewSession {
public:
    struct BeginResult {
        UiPreviewError error = UiPreviewError::OK;
        uint32_t token = 0;
        /// Whether this call opened the session, as against renewing or repeating one.
        bool is_created = false;
    };

    struct StageResult {
        UiPreviewError error = UiPreviewError::OK;
        /// The bytes received so far, or, for BAD_OFFSET, where the client resumes.
        uint32_t received = 0;
        /// The last of the data arrived.
        bool is_complete = false;
        /// For STALE: the revision applied.
        uint32_t rev = 0;
    };

    struct Snapshot {
        bool is_active = false;
        uint32_t rev = 0;
        int64_t lease_left_ms = 0;
        bool has_applied = false;
    };

    /** @brief What BeginPatch() decided. */
    struct PatchBegin {
        UiPreviewError error = UiPreviewError::OK;
        /// The revision applied, for STALE.
        uint32_t rev = 0;
    };

    /** @brief What closing a session found. */
    struct EndResult {
        /// False while a staged configuration is being applied: the session stays, and the end is asked again.
        bool is_ended = false;
        /// The screens were replaced by a staged configuration (even one that failed half way), so the stored ones
        /// have to come back.
        bool had_replaced_screens = false;
        /// SHOW moved navigation.
        bool had_navigated = false;
    };

private:
    mutable k_mutex lock_{};
    std::pmr::memory_resource* resource_;
    int64_t lease_ms_;

    uint32_t token_ = 0; // 0 while no session is open
    bool is_pinned_ = true;
    // Until a request that carries the token: a BEGIN sent again, because its answer got lost, gets the same one.
    bool is_fresh_ = false;
    int64_t last_activity_ms_ = 0;
    uint32_t rev_ = 0;
    // The screens are not the stored ones: set before a staged configuration is shown, cleared once the stored
    // ones are back. A rev only counts a configuration that was shown whole.
    bool has_applied_ = false;
    bool has_navigated_ = false;
    // From the last chunk to the end of the apply: the buffer is read meanwhile, so the session cannot end.
    bool is_applying_ = false;

    std::pmr::vector<uint8_t> buffer_;
    size_t capacity_ = 0;

    bool is_staging_ = false;
    uint32_t length_ = 0;
    uint32_t crc_ = 0;
    uint32_t received_ = 0;
    std::optional<uint32_t> requested_rev_;

    UiPreviewError TouchLocked(uint32_t token, int64_t now_ms);
    void CloseLocked();

public:
    /** @param lease_ms How long a session lives without a request that carries its token. */
    UiPreviewSession(int64_t lease_ms, std::pmr::memory_resource* resource);

    UiPreviewSession(const UiPreviewSession&) = delete;
    UiPreviewSession& operator=(const UiPreviewSession&) = delete;
    UiPreviewSession(UiPreviewSession&&) = delete;
    UiPreviewSession& operator=(UiPreviewSession&&) = delete;
    ~UiPreviewSession() = default;

    [[nodiscard]] int64_t GetLeaseMs() const {
        return lease_ms_;
    }

    /**
     * @brief Opens a session, or renews one.
     *
     * @param token The session's, to renew it; a different one is BAD_TOKEN, and with no session open it is
     *        NO_SESSION (it ended: the client must not be given a new one without knowing). Without one, a
     *        session that is open is BUSY, unless this repeats the BEGIN that opened it (no request has carried the
     *        token since, and @p is_pinned is the same): the client's answer got lost, and it gets the same token.
     * @param new_token What a new session is given; not 0.
     */
    BeginResult Begin(std::optional<uint32_t> token, bool is_pinned, uint32_t new_token, int64_t now_ms);

    /** @brief Allocates the stage buffer of the open session; NO_MEMORY when the resource is out. Not from a handler. */
    UiPreviewError AllocateBuffer(size_t capacity);

    /** @brief Renews the lease of the session @p token names. NO_SESSION, or BAD_TOKEN. */
    UiPreviewError Touch(uint32_t token, int64_t now_ms);

    /**
     * @brief Takes the next chunk of a staged configuration.
     *
     * At @p offset 0 a transfer starts: @p length and @p crc say what it is, the optional @p rev is the
     * revision the client gives the configuration (it must be past the applied one, else STALE), and an earlier
     * unfinished transfer is dropped. Later chunks must follow the bytes received (else BAD_OFFSET with where to
     * resume) and not pass the length (TOO_LARGE).
     */
    StageResult Stage(
        uint32_t token,
        uint32_t offset,
        std::span<const uint8_t> data,
        std::optional<uint32_t> length,
        std::optional<uint32_t> crc,
        std::optional<uint32_t> rev,
        int64_t now_ms
    );

    /** @brief The bytes of a complete transfer; empty before. Valid until the next Stage() or End(). */
    [[nodiscard]] std::span<const uint8_t> Staged() const;
    /** @brief The CRC-32 the client gave the transfer at offset 0. */
    [[nodiscard]] uint32_t GetStagedCrc() const;
    /** @brief Drops the transfer, after it was refused. */
    void DropStage();

    /** @brief The staged configuration is showing: its revision is the client's, else the next. Returns it. */
    uint32_t CommitStage();

    /**
     * @brief A patch is about to be applied: renews the lease and, if @p rev is past the applied one, marks the
     *        session applying until FinishApply(), as the last chunk of a stage does.
     * @return NO_SESSION, BAD_TOKEN, BUSY while a stage is under way (between its chunks too) or being applied, or
     *         another patch is, STALE (with the
     *         applied revision) for a revision that does not go forward.
     */
    PatchBegin BeginPatch(uint32_t token, uint32_t rev, int64_t now_ms);

    /** @brief The patch is shown: its revision is the applied one now. Returns it. */
    uint32_t CommitPatch(uint32_t rev);

    /** @brief The screens are about to be replaced by a staged configuration, and may not be the stored ones from now on. */
    void MarkScreensReplaced();
    /** @brief The stored screens are showing again. */
    void MarkScreensRestored();
    /** @brief SHOW moved navigation. */
    void MarkNavigated();

    /** @brief The last chunk's apply is over, whatever came of it. */
    void FinishApply();
    [[nodiscard]] bool IsApplying() const;

    /**
     * @brief Closes the session and frees the buffer, unless a staged configuration is being applied, which reads
     *        the buffer: then nothing changes and `is_ended` is false.
     */
    EndResult End();

    [[nodiscard]] bool IsActive() const;
    [[nodiscard]] bool IsPinned() const;
    [[nodiscard]] bool HasApplied() const;
    [[nodiscard]] uint32_t GetRev() const;
    [[nodiscard]] int64_t GetLeaseLeftMs(int64_t now_ms) const;
    [[nodiscard]] bool IsLeaseExpired(int64_t now_ms) const;
    [[nodiscard]] Snapshot GetSnapshot(int64_t now_ms) const;
};

} // namespace eerie_leap::domain::ui_domain::preview
