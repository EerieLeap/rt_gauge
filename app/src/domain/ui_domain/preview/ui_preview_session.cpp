#include <algorithm>
#include <new>

#include "subsys/threading/scoped_mutex.h"

#include "ui_preview_session.h"

namespace eerie_leap::domain::ui_domain::preview {

using eerie_leap::subsys::threading::ScopedMutex;

UiPreviewSession::UiPreviewSession(int64_t lease_ms, std::pmr::memory_resource* resource) :
    resource_(resource),
    lease_ms_(lease_ms),
    buffer_(resource) {

    k_mutex_init(&lock_);
}

UiPreviewSession::BeginResult
UiPreviewSession::Begin(std::optional<uint32_t> token, bool is_pinned, uint32_t new_token, int64_t now_ms) {
    const ScopedMutex guard(lock_);

    if(token_ == 0) {
        // A client that holds a token holds a session that ended: it learns that, and does not get another.
        if(token.has_value())
            return { .error = UiPreviewError::NO_SESSION };

        token_ = new_token;
        is_pinned_ = is_pinned;
        is_fresh_ = true;
        last_activity_ms_ = now_ms;
        rev_ = 0;
        has_applied_ = false;
        has_navigated_ = false;
        is_applying_ = false;
        is_staging_ = false;

        return { .token = token_, .is_created = true };
    }

    if(token.has_value()) {
        if(*token != token_)
            return { .error = UiPreviewError::BAD_TOKEN };

        is_fresh_ = false;
        last_activity_ms_ = now_ms;

        return { .token = token_ };
    }

    // Nobody has used the session: the BEGIN that opened it was answered, but the answer did not arrive.
    if(is_fresh_ && is_pinned == is_pinned_) {
        last_activity_ms_ = now_ms;

        return { .token = token_ };
    }

    return { .error = UiPreviewError::BUSY };
}

UiPreviewError UiPreviewSession::AllocateBuffer(size_t capacity) {
    const ScopedMutex guard(lock_);

    if(token_ == 0)
        return UiPreviewError::NO_SESSION;

    try {
        buffer_.assign(capacity, 0);
    } catch(const std::bad_alloc&) {
        return UiPreviewError::NO_MEMORY;
    }

    capacity_ = capacity;

    return UiPreviewError::OK;
}

UiPreviewError UiPreviewSession::TouchLocked(uint32_t token, int64_t now_ms) {
    if(token_ == 0)
        return UiPreviewError::NO_SESSION;

    if(token != token_)
        return UiPreviewError::BAD_TOKEN;

    is_fresh_ = false;
    last_activity_ms_ = now_ms;

    return UiPreviewError::OK;
}

UiPreviewError UiPreviewSession::Touch(uint32_t token, int64_t now_ms) {
    const ScopedMutex guard(lock_);

    return TouchLocked(token, now_ms);
}

UiPreviewSession::StageResult UiPreviewSession::Stage(
    uint32_t token,
    uint32_t offset,
    std::span<const uint8_t> data,
    std::optional<uint32_t> length,
    std::optional<uint32_t> crc,
    std::optional<uint32_t> rev,
    int64_t now_ms
) {

    const ScopedMutex guard(lock_);

    if(const auto error = TouchLocked(token, now_ms); error != UiPreviewError::OK)
        return { .error = error };

    // The buffer is being read by the apply of the last chunk: nothing may write it until that is over.
    if(is_applying_)
        return { .error = UiPreviewError::BUSY };

    if(offset == 0) {
        if(!length.has_value() || !crc.has_value())
            return { .error = UiPreviewError::UNKNOWN };

        is_staging_ = false;

        if(rev.has_value() && *rev <= rev_)
            return { .error = UiPreviewError::STALE, .rev = rev_ };

        if(*length == 0 || *length > capacity_)
            return { .error = UiPreviewError::TOO_LARGE };

        is_staging_ = true;
        length_ = *length;
        crc_ = *crc;
        received_ = 0;
        requested_rev_ = rev;
    } else if(!is_staging_ || offset != received_) {
        // A client whose answer got lost learns where to resume.
        return { .error = UiPreviewError::BAD_OFFSET, .received = is_staging_ ? received_ : 0 };
    }

    if(data.size() > length_ - received_) {
        is_staging_ = false;

        return { .error = UiPreviewError::TOO_LARGE };
    }

    std::ranges::copy(data, buffer_.begin() + static_cast<std::ptrdiff_t>(received_));
    received_ += static_cast<uint32_t>(data.size());

    const bool is_complete = received_ == length_;
    // Under the same lock as the last copy, so an end cannot slip in before the apply has begun.
    if(is_complete)
        is_applying_ = true;

    return { .received = received_, .is_complete = is_complete };
}

std::span<const uint8_t> UiPreviewSession::Staged() const {
    const ScopedMutex guard(lock_);

    if(token_ == 0 || !is_staging_ || received_ != length_)
        return {};

    return { buffer_.data(), length_ };
}

uint32_t UiPreviewSession::GetStagedCrc() const {
    const ScopedMutex guard(lock_);

    return crc_;
}

void UiPreviewSession::DropStage() {
    const ScopedMutex guard(lock_);

    is_staging_ = false;
    received_ = 0;
}

UiPreviewSession::PatchBegin UiPreviewSession::BeginPatch(uint32_t token, uint32_t rev, int64_t now_ms) {
    const ScopedMutex guard(lock_);

    if(const auto error = TouchLocked(token, now_ms); error != UiPreviewError::OK)
        return { .error = error };

    if(is_applying_)
        return { .error = UiPreviewError::BUSY };

    if(rev <= rev_)
        return { .error = UiPreviewError::STALE, .rev = rev_ };

    is_applying_ = true;

    return { .rev = rev_ };
}

uint32_t UiPreviewSession::CommitPatch(uint32_t rev) {
    const ScopedMutex guard(lock_);

    rev_ = rev;

    return rev_;
}

uint32_t UiPreviewSession::CommitStage() {
    const ScopedMutex guard(lock_);

    rev_ = requested_rev_.value_or(rev_ + 1);
    is_staging_ = false;
    received_ = 0;

    return rev_;
}

void UiPreviewSession::MarkScreensReplaced() {
    const ScopedMutex guard(lock_);

    has_applied_ = true;
}

void UiPreviewSession::MarkScreensRestored() {
    const ScopedMutex guard(lock_);

    has_applied_ = false;
}

void UiPreviewSession::MarkNavigated() {
    const ScopedMutex guard(lock_);

    has_navigated_ = true;
}

void UiPreviewSession::FinishApply() {
    const ScopedMutex guard(lock_);

    is_applying_ = false;
}

bool UiPreviewSession::IsApplying() const {
    const ScopedMutex guard(lock_);

    return is_applying_;
}

void UiPreviewSession::CloseLocked() {
    token_ = 0;
    has_applied_ = false;
    has_navigated_ = false;
    is_applying_ = false;
    is_staging_ = false;
    received_ = 0;
    rev_ = 0;
    capacity_ = 0;

    std::pmr::vector<uint8_t> empty(resource_);
    buffer_.swap(empty);
}

UiPreviewSession::EndResult UiPreviewSession::End() {
    const ScopedMutex guard(lock_);

    if(is_applying_)
        return {};

    const EndResult result{ .is_ended = true, .had_replaced_screens = has_applied_, .had_navigated = has_navigated_ };
    CloseLocked();

    return result;
}

bool UiPreviewSession::IsActive() const {
    const ScopedMutex guard(lock_);

    return token_ != 0;
}

bool UiPreviewSession::IsPinned() const {
    const ScopedMutex guard(lock_);

    return token_ != 0 && is_pinned_;
}

bool UiPreviewSession::HasApplied() const {
    const ScopedMutex guard(lock_);

    return has_applied_;
}

uint32_t UiPreviewSession::GetRev() const {
    const ScopedMutex guard(lock_);

    return rev_;
}

int64_t UiPreviewSession::GetLeaseLeftMs(int64_t now_ms) const {
    const ScopedMutex guard(lock_);

    return token_ == 0 ? 0 : std::max<int64_t>(0, lease_ms_ - (now_ms - last_activity_ms_));
}

bool UiPreviewSession::IsLeaseExpired(int64_t now_ms) const {
    const ScopedMutex guard(lock_);

    return token_ != 0 && now_ms - last_activity_ms_ >= lease_ms_;
}

UiPreviewSession::Snapshot UiPreviewSession::GetSnapshot(int64_t now_ms) const {
    const ScopedMutex guard(lock_);

    if(token_ == 0)
        return {};

    return { .is_active = true,
        .rev = rev_,
        .lease_left_ms = std::max<int64_t>(0, lease_ms_ - (now_ms - last_activity_ms_)),
        .has_applied = has_applied_ };
}

} // namespace eerie_leap::domain::ui_domain::preview
