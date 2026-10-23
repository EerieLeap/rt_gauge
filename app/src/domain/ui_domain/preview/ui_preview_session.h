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
    bool has_applied_ = false;

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
     * @param token The session's, to renew it; a different one is BAD_TOKEN. Without one, a session that is
     *        open is BUSY, unless this repeats the BEGIN that opened it (no request has carried the token
     *        since, and @p is_pinned is the same): the client's answer got lost, and it gets the same token.
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

    /** @brief Closes the session and frees the buffer; whether a staged configuration was showing. */
    bool End();

    [[nodiscard]] bool IsActive() const;
    [[nodiscard]] bool IsPinned() const;
    [[nodiscard]] bool HasApplied() const;
    [[nodiscard]] uint32_t GetRev() const;
    [[nodiscard]] int64_t GetLeaseLeftMs(int64_t now_ms) const;
    [[nodiscard]] bool IsLeaseExpired(int64_t now_ms) const;
    [[nodiscard]] Snapshot GetSnapshot(int64_t now_ms) const;
};

} // namespace eerie_leap::domain::ui_domain::preview
