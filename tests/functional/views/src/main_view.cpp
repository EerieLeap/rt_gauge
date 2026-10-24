#include <cerrno>
#include <functional>
#include <memory>
#include <vector>

#include <lvgl.h>
#include <zephyr/ztest.h>

#include "views/main_view.h"

#include "views_test_support.h"

using eerie_leap::views::MainView;
using views_test::CleanTestDisplay;
using views_test::EnsureTestDisplay;
using views_test::FakeScreen;

namespace {

// The dispatcher stands in for the UI render work queue: the work is collected
// here so a test can decide exactly when - and in which order - it runs.
class DeferredRenderQueue {
private:
    std::vector<std::function<void()>> work_;

public:
    MainView::RenderDispatcher GetDispatcher() {
        return [this](std::function<void()> work) {
            work_.push_back(std::move(work));
        };
    }

    size_t Pending() const {
        return work_.size();
    }

    void RunAll() {
        auto work = std::move(work_);
        work_.clear();

        for(auto& item : work)
            item();
    }
};

std::shared_ptr<FakeScreen> AddScreen(MainView& view, uint32_t id, uint32_t screen_group_id) {
    auto screen = std::make_shared<FakeScreen>(id, screen_group_id, 0, true, view.GetGroupContainer(screen_group_id));
    view.AddScreen(screen);

    return screen;
}

bool IsGroupHidden(MainView& view, uint32_t screen_group_id) {
    return lv_obj_is_hidden(view.GetGroupContainer(screen_group_id)->GetObject());
}

void* SetUp() {
    EnsureTestDisplay();

    return nullptr;
}

} // namespace

ZTEST_SUITE(main_view, NULL, SetUp, NULL, CleanTestDisplay, NULL);

ZTEST(main_view, test_an_unknown_group_is_rejected) {
    MainView view;

    zassert_equal(view.SetActiveGroup(3), -ENOENT);
    zassert_false(view.GetActiveGroupId().has_value());
}

// Configure() runs before Start(), so the selection has to survive until the
// view itself is rendered.
ZTEST(main_view, test_a_selection_made_before_rendering_is_only_recorded) {
    MainView view;

    auto screen = AddScreen(view, 0, 0);

    zassert_equal(view.SetActiveGroup(0), 0);
    zassert_equal(view.GetActiveGroupId().value(), 0);
    zassert_equal(screen->render_count, 0);

    zassert_equal(view.Render(), 0);

    zassert_equal(screen->render_count, 1);
    zassert_equal(screen->activated_count, 1);
}

ZTEST(main_view, test_only_the_active_group_is_rendered) {
    MainView view;

    auto first = AddScreen(view, 0, 0);
    auto second = AddScreen(view, 1, 1);

    zassert_equal(view.SetActiveGroup(0), 0);
    zassert_equal(view.Render(), 0);

    zassert_equal(first->render_count, 1);
    zassert_equal(second->render_count, 0);
}

ZTEST(main_view, test_without_a_dispatcher_the_switch_renders_inline) {
    MainView view;

    auto first = AddScreen(view, 0, 0);
    auto second = AddScreen(view, 1, 1);

    zassert_equal(view.SetActiveGroup(0), 0);
    zassert_equal(view.Render(), 0);

    zassert_equal(view.SetActiveGroup(1), 0);

    zassert_equal(second->render_count, 1);
    zassert_equal(second->activated_count, 1);
    zassert_equal(first->deactivated_count, 1);
    zassert_equal(view.GetActiveGroupId().value(), 1);
}

ZTEST(main_view, test_reselecting_the_active_group_is_a_no_op) {
    MainView view;

    auto screen = AddScreen(view, 0, 0);

    zassert_equal(view.SetActiveGroup(0), 0);
    zassert_equal(view.Render(), 0);
    zassert_equal(view.SetActiveGroup(0), 0);

    zassert_equal(screen->render_count, 1);
    zassert_equal(screen->activated_count, 1);
    zassert_equal(screen->deactivated_count, 0);
}

// The first render of a group builds its whole LVGL tree; doing that on the
// event bus thread would block the renderer through the LVGL lock.
ZTEST(main_view, test_with_a_dispatcher_the_first_render_is_deferred) {
    MainView view;
    DeferredRenderQueue queue;

    auto first = AddScreen(view, 0, 0);
    auto second = AddScreen(view, 1, 1);

    zassert_equal(view.SetActiveGroup(0), 0);
    zassert_equal(view.Render(), 0);

    view.SetRenderDispatcher(queue.GetDispatcher());

    zassert_equal(view.SetActiveGroup(1), 0);

    // The switch is already committed even though nothing has been built yet.
    zassert_equal(view.GetActiveGroupId().value(), 1);
    zassert_equal(first->deactivated_count, 1);
    zassert_equal(second->render_count, 0);
    zassert_equal(queue.Pending(), 1);

    queue.RunAll();

    zassert_equal(second->render_count, 1);
    zassert_equal(second->activated_count, 1);
}

ZTEST(main_view, test_an_already_rendered_group_does_not_queue_work) {
    MainView view;
    DeferredRenderQueue queue;

    auto first = AddScreen(view, 0, 0);
    auto second = AddScreen(view, 1, 1);

    zassert_equal(view.SetActiveGroup(0), 0);
    zassert_equal(view.Render(), 0);

    view.SetRenderDispatcher(queue.GetDispatcher());

    zassert_equal(view.SetActiveGroup(1), 0);
    queue.RunAll();

    zassert_equal(view.SetActiveGroup(0), 0);
    zassert_equal(queue.Pending(), 0);
    zassert_equal(first->activated_count, 2);
    zassert_equal(first->render_count, 1);
}

// Two swipes before either render completes: the stale one must not resurface.
ZTEST(main_view, test_back_to_back_switches_leave_only_the_last_group_visible) {
    MainView view;
    DeferredRenderQueue queue;

    auto first = AddScreen(view, 0, 0);
    auto second = AddScreen(view, 1, 1);
    auto third = AddScreen(view, 2, 2);

    zassert_equal(view.SetActiveGroup(0), 0);
    zassert_equal(view.Render(), 0);

    view.SetRenderDispatcher(queue.GetDispatcher());

    zassert_equal(view.SetActiveGroup(1), 0);
    zassert_equal(view.SetActiveGroup(2), 0);
    zassert_equal(queue.Pending(), 2);

    queue.RunAll();

    zassert_equal(view.GetActiveGroupId().value(), 2);
    zassert_equal(second->render_count, 1);
    zassert_equal(second->activated_count, 0);
    zassert_equal(third->activated_count, 1);
    zassert_true(IsGroupHidden(view, 1));
    zassert_false(IsGroupHidden(view, 2));
}

ZTEST(main_view, test_empty_groups_are_pruned) {
    MainView view;

    AddScreen(view, 0, 0);

    // A screen that fails to configure still leaves its group behind.
    view.GetGroupContainer(9);

    zassert_equal(view.GetGroupIds().size(), 2);

    view.PruneEmptyGroups();

    auto screen_group_ids = view.GetGroupIds();
    zassert_equal(screen_group_ids.size(), 1);
    zassert_equal(screen_group_ids[0], 0);
}

ZTEST(main_view, test_group_ids_are_sorted) {
    MainView view;

    AddScreen(view, 0, 7);
    AddScreen(view, 1, 2);
    AddScreen(view, 2, 5);

    auto screen_group_ids = view.GetGroupIds();

    zassert_equal(screen_group_ids.size(), 3);
    zassert_equal(screen_group_ids[0], 2);
    zassert_equal(screen_group_ids[1], 5);
    zassert_equal(screen_group_ids[2], 7);
}

ZTEST(main_view, test_screens_are_found_across_groups) {
    MainView view;

    auto first = AddScreen(view, 4, 0);
    auto second = AddScreen(view, 5, 1);

    zassert_equal(view.GetScreen(4).get(), first.get());
    zassert_equal(view.GetScreen(5).get(), second.get());
    zassert_is_null(view.GetScreen(6).get());
}

ZTEST(main_view, test_a_failed_inline_render_rolls_back_to_the_previous_group) {
    MainView view;

    auto first = AddScreen(view, 0, 0);
    auto second = AddScreen(view, 1, 1);
    second->FailNextRenders();

    zassert_equal(view.SetActiveGroup(0), 0);
    zassert_equal(view.Render(), 0);

    zassert_not_equal(view.SetActiveGroup(1), 0);

    zassert_equal(view.GetActiveGroupId().value(), 0);
    zassert_equal(first->activated_count, 2);
    zassert_false(IsGroupHidden(view, 0));
    zassert_true(IsGroupHidden(view, 1));
}

// The deferred switch is already committed when the render runs, so the failure
// has to be undone afterwards or the screen stays blank.
ZTEST(main_view, test_a_failed_deferred_render_rolls_back_to_the_previous_group) {
    MainView view;
    DeferredRenderQueue queue;

    auto first = AddScreen(view, 0, 0);
    auto second = AddScreen(view, 1, 1);
    second->FailNextRenders();

    zassert_equal(view.SetActiveGroup(0), 0);
    zassert_equal(view.Render(), 0);

    view.SetRenderDispatcher(queue.GetDispatcher());

    zassert_equal(view.SetActiveGroup(1), 0);
    zassert_equal(view.GetActiveGroupId().value(), 1);

    queue.RunAll();

    zassert_equal(view.GetActiveGroupId().value(), 0);
    zassert_equal(first->activated_count, 2);
    zassert_false(IsGroupHidden(view, 0));
    zassert_true(IsGroupHidden(view, 1));
}

// A second switch queued behind the failing one already owns the active state.
ZTEST(main_view, test_a_failed_deferred_render_does_not_undo_a_later_switch) {
    MainView view;
    DeferredRenderQueue queue;

    auto first = AddScreen(view, 0, 0);
    auto second = AddScreen(view, 1, 1);
    auto third = AddScreen(view, 2, 2);
    second->FailNextRenders();

    zassert_equal(view.SetActiveGroup(0), 0);
    zassert_equal(view.Render(), 0);

    view.SetRenderDispatcher(queue.GetDispatcher());

    zassert_equal(view.SetActiveGroup(1), 0);
    zassert_equal(view.SetActiveGroup(2), 0);

    queue.RunAll();

    zassert_equal(view.GetActiveGroupId().value(), 2);
    zassert_equal(third->render_count, 1);
    zassert_false(IsGroupHidden(view, 2));
    zassert_true(IsGroupHidden(view, 0));
    zassert_true(IsGroupHidden(view, 1));
}

// The work queue keeps a finished task, and what it captured, until a later Run() prunes it without the LVGL
// lock; a group it kept alive past Clear() would be destroyed there, under the queue's lock.
ZTEST(main_view, test_a_pending_render_keeps_no_group_alive_past_clear) {
    MainView view;
    DeferredRenderQueue queue;

    auto first = AddScreen(view, 0, 0);
    auto second = AddScreen(view, 1, 1);
    zassert_equal(view.SetActiveGroup(0), 0);
    zassert_equal(view.Render(), 0);
    view.SetRenderDispatcher(queue.GetDispatcher());

    zassert_equal(view.SetActiveGroup(1), 0);
    zassert_equal(queue.Pending(), 1);

    std::weak_ptr<FakeScreen> first_alive = first;
    std::weak_ptr<FakeScreen> second_alive = second;
    first.reset();
    second.reset();
    zassert_false(first_alive.expired(), "The view still holds its groups.");

    view.Clear();

    zassert_true(first_alive.expired(), "The previous group went with Clear().");
    zassert_true(second_alive.expired(), "The group waiting to be rendered went with Clear().");
    zassert_false(view.GetActiveGroupId().has_value());

    // The work that was queued finds nothing to render.
    queue.RunAll();
    zassert_false(view.GetActiveGroupId().has_value());
}

// A group dropped and a new one built under the same id: the old work must not render the new group as its own.
ZTEST(main_view, test_a_pending_render_of_a_dropped_group_does_not_touch_the_new_one) {
    MainView view;
    DeferredRenderQueue queue;

    auto first = AddScreen(view, 0, 0);
    auto second = AddScreen(view, 1, 1);
    zassert_equal(view.SetActiveGroup(0), 0);
    zassert_equal(view.Render(), 0);
    view.SetRenderDispatcher(queue.GetDispatcher());
    zassert_equal(view.SetActiveGroup(1), 0);
    zassert_equal(queue.Pending(), 1);

    view.Clear();
    first.reset();
    second.reset();

    auto replacement = AddScreen(view, 1, 1);
    zassert_equal(view.SetActiveGroup(1, true), 0);
    zassert_equal(replacement->render_count, 1, "Rendered inline once.");

    queue.RunAll();

    zassert_equal(replacement->render_count, 1, "The old work did not render it again.");
    zassert_equal(view.GetActiveGroupId().value(), 1);
}

ZTEST(main_view, test_an_inline_switch_renders_at_once_despite_a_dispatcher) {
    MainView view;
    DeferredRenderQueue queue;

    auto first = AddScreen(view, 0, 0);
    auto second = AddScreen(view, 1, 1);
    zassert_equal(view.SetActiveGroup(0), 0);
    zassert_equal(view.Render(), 0);
    view.SetRenderDispatcher(queue.GetDispatcher());

    zassert_equal(view.SetActiveGroup(1, true), 0);

    zassert_equal(queue.Pending(), 0);
    zassert_equal(second->render_count, 1);
    zassert_equal(second->activated_count, 1);
}
