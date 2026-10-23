#include <algorithm>
#include <array>
#include <cstdint>
#include <memory_resource>
#include <optional>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/ui_domain/preview/ui_preview_session.h"

using eerie_leap::domain::ui_domain::preview::UiPreviewError;
using eerie_leap::domain::ui_domain::preview::UiPreviewSession;

namespace {

constexpr int64_t LEASE_MS = 5000;
constexpr uint32_t TOKEN = 0xA1B2C3D4;
constexpr size_t CAPACITY = 1024;
constexpr uint32_t CRC = 0x1234;

std::vector<uint8_t> Bytes(size_t size) {
    std::vector<uint8_t> bytes(size);
    for(size_t i = 0; i < size; ++i)
        bytes[i] = static_cast<uint8_t>(i + 1);

    return bytes;
}

// A session that has begun at time 0, with its buffer.
struct Open {
    UiPreviewSession session{ LEASE_MS, std::pmr::get_default_resource() };

    Open() {
        const auto begun = session.Begin(std::nullopt, true, TOKEN, 0);
        zassert_equal(begun.error, UiPreviewError::OK);
        zassert_equal(session.AllocateBuffer(CAPACITY), UiPreviewError::OK);
    }
};

} // namespace

ZTEST_SUITE(ui_preview_session, NULL, NULL, NULL, NULL, NULL);

ZTEST(ui_preview_session, test_a_session_opens_with_the_token_it_is_given) {
    UiPreviewSession session(LEASE_MS, std::pmr::get_default_resource());
    zassert_false(session.IsActive());

    const auto begun = session.Begin(std::nullopt, true, TOKEN, 100);

    zassert_equal(begun.error, UiPreviewError::OK);
    zassert_equal(begun.token, TOKEN);
    zassert_true(begun.is_created);
    zassert_true(session.IsActive());
    zassert_true(session.IsPinned());
    zassert_equal(session.GetRev(), 0U);
    zassert_false(session.HasApplied());
}

ZTEST(ui_preview_session, test_the_pin_is_the_clients_choice) {
    UiPreviewSession session(LEASE_MS, std::pmr::get_default_resource());

    zassert_equal(session.Begin(std::nullopt, false, TOKEN, 0).error, UiPreviewError::OK);

    zassert_false(session.IsPinned());
}

ZTEST(ui_preview_session, test_a_begin_with_the_token_renews_and_one_with_another_is_refused) {
    Open open;

    const auto renewed = open.session.Begin(TOKEN, true, 0xDEAD, 4000);
    zassert_equal(renewed.error, UiPreviewError::OK);
    zassert_equal(renewed.token, TOKEN);
    zassert_false(renewed.is_created);
    zassert_equal(open.session.GetLeaseLeftMs(4000), LEASE_MS, "The lease starts over");

    zassert_equal(open.session.Begin(TOKEN + 1, true, 0xDEAD, 4000).error, UiPreviewError::BAD_TOKEN);
}

// The answer to the BEGIN that opened it got lost: asking again gets the same one, once, until the token is used.
ZTEST(ui_preview_session, test_a_begin_repeated_before_the_token_was_used_gets_the_same_token) {
    Open open;

    const auto repeated = open.session.Begin(std::nullopt, true, 0xDEAD, 10);
    zassert_equal(repeated.error, UiPreviewError::OK);
    zassert_equal(repeated.token, TOKEN);
    zassert_false(repeated.is_created);

    // Not the same request: another pin.
    zassert_equal(open.session.Begin(std::nullopt, false, 0xDEAD, 10).error, UiPreviewError::BUSY);

    zassert_equal(open.session.Touch(TOKEN, 20), UiPreviewError::OK);
    zassert_equal(open.session.Begin(std::nullopt, true, 0xDEAD, 30).error, UiPreviewError::BUSY, "Used since");
}

ZTEST(ui_preview_session, test_touch_renews_the_lease_of_the_token_it_is_given) {
    Open open;

    zassert_equal(open.session.GetLeaseLeftMs(1000), LEASE_MS - 1000);
    zassert_equal(open.session.Touch(TOKEN, 1000), UiPreviewError::OK);
    zassert_equal(open.session.GetLeaseLeftMs(1000), LEASE_MS);

    zassert_equal(open.session.Touch(TOKEN + 1, 2000), UiPreviewError::BAD_TOKEN);
    zassert_equal(open.session.GetLeaseLeftMs(2000), LEASE_MS - 1000, "A wrong token renews nothing");

    UiPreviewSession closed(LEASE_MS, std::pmr::get_default_resource());
    zassert_equal(closed.Touch(TOKEN, 0), UiPreviewError::NO_SESSION);
}

ZTEST(ui_preview_session, test_the_lease_runs_out_after_the_time_without_a_request) {
    Open open;

    zassert_false(open.session.IsLeaseExpired(LEASE_MS - 1));
    zassert_true(open.session.IsLeaseExpired(LEASE_MS));
    zassert_equal(open.session.GetLeaseLeftMs(LEASE_MS + 10), 0);

    UiPreviewSession closed(LEASE_MS, std::pmr::get_default_resource());
    zassert_false(closed.IsLeaseExpired(LEASE_MS * 10), "No session, no lease");
    zassert_equal(closed.GetLeaseLeftMs(0), 0);
}

ZTEST(ui_preview_session, test_the_buffer_needs_a_session) {
    UiPreviewSession session(LEASE_MS, std::pmr::get_default_resource());

    zassert_equal(session.AllocateBuffer(CAPACITY), UiPreviewError::NO_SESSION);
}

ZTEST(ui_preview_session, test_chunks_in_order_make_a_staged_configuration) {
    Open open;
    const auto bytes = Bytes(300);
    const std::span<const uint8_t> all(bytes);

    auto first = open.session.Stage(TOKEN, 0, all.first(100), 300, CRC, std::nullopt, 10);
    zassert_equal(first.error, UiPreviewError::OK);
    zassert_equal(first.received, 100U);
    zassert_false(first.is_complete);
    zassert_true(open.session.Staged().empty(), "Not complete yet");

    auto second = open.session.Stage(TOKEN, 100, all.subspan(100, 100), std::nullopt, std::nullopt, std::nullopt, 20);
    zassert_equal(second.received, 200U);
    zassert_false(second.is_complete);

    auto third = open.session.Stage(TOKEN, 200, all.subspan(200), std::nullopt, std::nullopt, std::nullopt, 30);
    zassert_equal(third.error, UiPreviewError::OK);
    zassert_true(third.is_complete);

    const auto staged = open.session.Staged();
    zassert_equal(staged.size(), 300U);
    zassert_true(std::equal(staged.begin(), staged.end(), bytes.begin()));
    zassert_equal(open.session.GetStagedCrc(), CRC);
    zassert_equal(open.session.GetLeaseLeftMs(30), LEASE_MS, "Every chunk renews the lease");
}

ZTEST(ui_preview_session, test_the_first_chunk_says_what_the_transfer_is) {
    Open open;
    const auto bytes = Bytes(10);

    zassert_equal(
        open.session.Stage(TOKEN, 0, bytes, std::nullopt, CRC, std::nullopt, 0).error, UiPreviewError::UNKNOWN,
        "No length"
    );
    zassert_equal(
        open.session.Stage(TOKEN, 0, bytes, 10, std::nullopt, std::nullopt, 0).error, UiPreviewError::UNKNOWN, "No CRC"
    );
    zassert_equal(open.session.Stage(TOKEN, 0, bytes, 0, CRC, std::nullopt, 0).error, UiPreviewError::TOO_LARGE);
    zassert_equal(
        open.session.Stage(TOKEN, 0, bytes, CAPACITY + 1, CRC, std::nullopt, 0).error, UiPreviewError::TOO_LARGE,
        "More than the buffer holds"
    );
    zassert_equal(open.session.Stage(TOKEN, 0, bytes, CAPACITY, CRC, std::nullopt, 0).error, UiPreviewError::OK);
}

ZTEST(ui_preview_session, test_a_chunk_out_of_order_says_where_to_resume) {
    Open open;
    const auto bytes = Bytes(200);

    zassert_equal(open.session.Stage(TOKEN, 0, std::span(bytes).first(100), 200, CRC, std::nullopt, 0).received, 100U);

    const auto skipped = open.session.Stage(
        TOKEN, 150, std::span(bytes).first(50), std::nullopt, std::nullopt, std::nullopt, 0
    );
    zassert_equal(skipped.error, UiPreviewError::BAD_OFFSET);
    zassert_equal(skipped.received, 100U);

    // Without a transfer to continue there is nothing to resume.
    Open fresh;
    const auto none = fresh.session.Stage(TOKEN, 50, bytes, std::nullopt, std::nullopt, std::nullopt, 0);
    zassert_equal(none.error, UiPreviewError::BAD_OFFSET);
    zassert_equal(none.received, 0U);
}

ZTEST(ui_preview_session, test_data_past_the_length_is_refused_and_ends_the_transfer) {
    Open open;
    const auto bytes = Bytes(100);

    zassert_equal(open.session.Stage(TOKEN, 0, bytes, 50, CRC, std::nullopt, 0).error, UiPreviewError::TOO_LARGE);
    zassert_equal(
        open.session.Stage(TOKEN, 0, std::span(bytes).first(50), std::nullopt, std::nullopt, std::nullopt, 0).error,
        UiPreviewError::UNKNOWN, "The first chunk restarts the transfer, and says what it is"
    );
}

ZTEST(ui_preview_session, test_a_chunk_needs_the_token) {
    Open open;
    const auto bytes = Bytes(10);

    zassert_equal(open.session.Stage(TOKEN + 1, 0, bytes, 10, CRC, std::nullopt, 0).error, UiPreviewError::BAD_TOKEN);

    UiPreviewSession closed(LEASE_MS, std::pmr::get_default_resource());
    zassert_equal(closed.Stage(TOKEN, 0, bytes, 10, CRC, std::nullopt, 0).error, UiPreviewError::NO_SESSION);
}

ZTEST(ui_preview_session, test_the_first_chunk_starts_a_transfer_over_an_unfinished_one) {
    Open open;
    const auto bytes = Bytes(100);

    zassert_equal(open.session.Stage(TOKEN, 0, std::span(bytes).first(40), 100, CRC, std::nullopt, 0).received, 40U);

    const auto again = open.session.Stage(TOKEN, 0, std::span(bytes).first(60), 60, CRC + 1, std::nullopt, 0);
    zassert_equal(again.error, UiPreviewError::OK);
    zassert_true(again.is_complete);
    zassert_equal(open.session.GetStagedCrc(), CRC + 1);
}

ZTEST(ui_preview_session, test_a_commit_counts_the_revision_or_takes_the_clients) {
    Open open;
    const auto bytes = Bytes(10);

    zassert_equal(open.session.Stage(TOKEN, 0, bytes, 10, CRC, std::nullopt, 0).error, UiPreviewError::OK);
    zassert_equal(open.session.CommitStage(), 1U);
    open.session.FinishApply();
    zassert_false(open.session.HasApplied(), "Only the screens being replaced say so, before they are");
    zassert_true(open.session.Staged().empty(), "The transfer is over");

    zassert_equal(open.session.Stage(TOKEN, 0, bytes, 10, CRC, 7, 0).error, UiPreviewError::OK);
    zassert_equal(open.session.CommitStage(), 7U);
    open.session.FinishApply();
    zassert_equal(open.session.GetRev(), 7U);

    zassert_equal(open.session.Stage(TOKEN, 0, bytes, 10, CRC, std::nullopt, 0).error, UiPreviewError::OK);
    zassert_equal(open.session.CommitStage(), 8U);
    open.session.FinishApply();
}

// A revision the client gives must be past the applied one, and tells the applied one when it is not.
ZTEST(ui_preview_session, test_a_revision_that_does_not_go_forward_is_stale) {
    Open open;
    const auto bytes = Bytes(10);

    zassert_equal(open.session.Stage(TOKEN, 0, bytes, 10, CRC, 4, 0).error, UiPreviewError::OK);
    open.session.CommitStage();
    open.session.FinishApply();

    for(const uint32_t rev : { 0U, 3U, 4U }) {
        const auto stale = open.session.Stage(TOKEN, 0, bytes, 10, CRC, rev, 0);
        zassert_equal(stale.error, UiPreviewError::STALE, "Revision %u", rev);
        zassert_equal(stale.rev, 4U);
    }

    zassert_equal(open.session.Stage(TOKEN, 0, bytes, 10, CRC, 5, 0).error, UiPreviewError::OK);
}

ZTEST(ui_preview_session, test_a_dropped_transfer_is_gone) {
    Open open;
    const auto bytes = Bytes(10);

    zassert_true(open.session.Stage(TOKEN, 0, bytes, 10, CRC, std::nullopt, 0).is_complete);
    open.session.DropStage();
    open.session.FinishApply();

    zassert_true(open.session.Staged().empty());
    zassert_equal(
        open.session.Stage(TOKEN, 10, bytes, std::nullopt, std::nullopt, std::nullopt, 0).error,
        UiPreviewError::BAD_OFFSET
    );
}

ZTEST(ui_preview_session, test_the_end_frees_the_session_and_says_whether_a_configuration_was_shown) {
    Open open;
    const auto bytes = Bytes(10);
    zassert_equal(open.session.Stage(TOKEN, 0, bytes, 10, CRC, std::nullopt, 0).error, UiPreviewError::OK);
    open.session.MarkScreensReplaced();
    open.session.CommitStage();
    open.session.FinishApply();

    const auto ended = open.session.End();
    zassert_true(ended.is_ended);
    zassert_true(ended.had_replaced_screens);
    zassert_false(ended.had_navigated);

    zassert_false(open.session.IsActive());
    zassert_false(open.session.HasApplied());
    zassert_equal(open.session.GetRev(), 0U);
    zassert_equal(open.session.Touch(TOKEN, 0), UiPreviewError::NO_SESSION);
    zassert_false(open.session.End().had_replaced_screens, "Nothing to end twice");

    // A new session is a new one, with its own token and revision.
    const auto next = open.session.Begin(std::nullopt, true, TOKEN + 9, 100);
    zassert_equal(next.token, TOKEN + 9);
    zassert_true(next.is_created);
    zassert_equal(open.session.AllocateBuffer(CAPACITY), UiPreviewError::OK);
}

ZTEST(ui_preview_session, test_the_snapshot_says_what_a_status_needs) {
    UiPreviewSession session(LEASE_MS, std::pmr::get_default_resource());
    zassert_false(session.GetSnapshot(0).is_active);

    zassert_equal(session.Begin(std::nullopt, true, TOKEN, 1000).error, UiPreviewError::OK);

    const auto snapshot = session.GetSnapshot(3000);
    zassert_true(snapshot.is_active);
    zassert_equal(snapshot.lease_left_ms, LEASE_MS - 2000);
    zassert_equal(snapshot.rev, 0U);
    zassert_false(snapshot.has_applied);
}

// The client that holds a token holds a session that ended: it must learn that, not be given another.
ZTEST(ui_preview_session, test_a_begin_with_a_token_and_no_session_open_is_refused) {
    UiPreviewSession session(LEASE_MS, std::pmr::get_default_resource());

    zassert_equal(session.Begin(TOKEN, true, TOKEN + 1, 0).error, UiPreviewError::NO_SESSION);
    zassert_false(session.IsActive());

    zassert_equal(session.Begin(std::nullopt, true, TOKEN, 0).error, UiPreviewError::OK);
    zassert_true(session.End().is_ended);
    zassert_equal(session.Begin(TOKEN, true, TOKEN + 1, 10).error, UiPreviewError::NO_SESSION);
    zassert_false(session.IsActive());
}

// The last chunk starts the apply under the lock that ends a session, so no end takes the buffer meanwhile.
ZTEST(ui_preview_session, test_a_session_does_not_end_while_the_last_chunk_is_applied) {
    Open open;
    const auto bytes = Bytes(10);

    zassert_false(open.session.IsApplying());
    zassert_true(open.session.Stage(TOKEN, 0, bytes, 10, CRC, std::nullopt, 0).is_complete);
    zassert_true(open.session.IsApplying());

    const auto refused = open.session.End();
    zassert_false(refused.is_ended);
    zassert_true(open.session.IsActive());
    zassert_equal(open.session.Staged().size(), 10U, "The buffer is still there to be read");

    // And nothing writes it.
    zassert_equal(open.session.Stage(TOKEN, 0, bytes, 10, CRC, std::nullopt, 0).error, UiPreviewError::BUSY);

    open.session.FinishApply();
    zassert_false(open.session.IsApplying());
    zassert_true(open.session.End().is_ended);
    zassert_false(open.session.IsActive());
}

ZTEST(ui_preview_session, test_a_chunk_that_is_not_the_last_does_not_start_an_apply) {
    Open open;
    const auto bytes = Bytes(10);

    zassert_false(open.session.Stage(TOKEN, 0, std::span(bytes).first(5), 10, CRC, std::nullopt, 0).is_complete);

    zassert_false(open.session.IsApplying());
    zassert_true(open.session.End().is_ended);
}

// What an end has to undo: the screens a staged configuration replaced, even one that failed half way, and a SHOW.
ZTEST(ui_preview_session, test_the_end_says_what_it_has_to_undo) {
    Open open;
    zassert_false(open.session.End().had_replaced_screens);

    Open replaced;
    replaced.session.MarkScreensReplaced();
    zassert_true(replaced.session.HasApplied());
    const auto after_replace = replaced.session.End();
    zassert_true(after_replace.had_replaced_screens);
    zassert_false(after_replace.had_navigated);

    Open restored;
    restored.session.MarkScreensReplaced();
    restored.session.MarkScreensRestored();
    zassert_false(restored.session.HasApplied());
    zassert_false(restored.session.End().had_replaced_screens);

    Open navigated;
    navigated.session.MarkNavigated();
    const auto after_navigation = navigated.session.End();
    zassert_true(after_navigation.had_navigated);
    zassert_false(after_navigation.had_replaced_screens);

    // Nothing carries over to the next session.
    const auto next = navigated.session.Begin(std::nullopt, true, TOKEN + 3, 0);
    zassert_equal(next.error, UiPreviewError::OK);
    const auto fresh = navigated.session.End();
    zassert_false(fresh.had_navigated);
    zassert_false(fresh.had_replaced_screens);
}
