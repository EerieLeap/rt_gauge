#include <memory>

#include <zephyr/ztest.h>
#include <lvgl.h>

#include "views/screens/screen_group.h"
#include "views/utilitites/frame.h"

#include "views_test_support.h"

using eerie_leap::views::screens::ScreenGroup;
using eerie_leap::views::utilitites::Frame;
using views_test::CleanTestDisplay;
using views_test::EnsureTestDisplay;
using views_test::FakeScreen;

namespace {

std::shared_ptr<Frame> MakeRoot() {
    return std::make_shared<Frame>(Frame::CreateWrapped()
        .SetWidth(100, false)
        .SetHeight(100, false)
        .Build());
}

bool IsHidden(const std::shared_ptr<ScreenGroup>& group) {
    return lv_obj_has_flag(group->GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN);
}

void* SetUp() {
    EnsureTestDisplay();

    return nullptr;
}

} // namespace

ZTEST_SUITE(screen_group, NULL, SetUp, NULL, CleanTestDisplay, NULL);

ZTEST(screen_group, test_a_new_group_is_hidden_and_unrendered) {
    auto root = MakeRoot();
    auto group = std::make_shared<ScreenGroup>(4, root);

    zassert_equal(group->GetGroupId(), 4);
    zassert_true(group->IsEmpty());
    zassert_false(group->IsRendered());
    zassert_false(group->IsActivated());
    zassert_true(IsHidden(group));
}

ZTEST(screen_group, test_ensure_rendered_renders_the_screens_once) {
    auto root = MakeRoot();
    auto group = std::make_shared<ScreenGroup>(0, root);

    auto screen = std::make_shared<FakeScreen>(1, 0, 0, true, group->GetContainer());
    group->AddScreen(screen);

    zassert_equal(group->EnsureRendered(), 0);
    zassert_equal(group->EnsureRendered(), 0);

    zassert_true(group->IsRendered());
    zassert_equal(screen->render_count, 1);
}

ZTEST(screen_group, test_a_failing_screen_leaves_the_group_unrendered) {
    auto root = MakeRoot();
    auto group = std::make_shared<ScreenGroup>(0, root);

    auto screen = std::make_shared<FakeScreen>(1, 0, 0, true, group->GetContainer());
    screen->FailNextRenders();
    group->AddScreen(screen);

    zassert_not_equal(group->EnsureRendered(), 0);
    zassert_false(group->IsRendered());
    zassert_true(IsHidden(group));
}

// Activation is requested synchronously, but the LVGL tree may only be built
// later on the render work queue.
ZTEST(screen_group, test_activation_requested_before_rendering_is_applied_on_render) {
    auto root = MakeRoot();
    auto group = std::make_shared<ScreenGroup>(0, root);

    auto screen = std::make_shared<FakeScreen>(1, 0, 0, true, group->GetContainer());
    group->AddScreen(screen);

    group->Activate();

    zassert_false(group->IsActivated());
    zassert_equal(screen->activated_count, 0);
    zassert_true(IsHidden(group));

    zassert_equal(group->EnsureRendered(), 0);

    zassert_true(group->IsActivated());
    zassert_equal(screen->activated_count, 1);
    zassert_false(IsHidden(group));
}

// Two swipes in a row: the first group's queued render must not steal the
// screen back once it completes.
ZTEST(screen_group, test_deactivation_cancels_a_pending_activation) {
    auto root = MakeRoot();
    auto group = std::make_shared<ScreenGroup>(0, root);

    auto screen = std::make_shared<FakeScreen>(1, 0, 0, true, group->GetContainer());
    group->AddScreen(screen);

    group->Activate();
    group->Deactivate();

    zassert_equal(group->EnsureRendered(), 0);

    zassert_false(group->IsActivated());
    zassert_equal(screen->activated_count, 0);
    zassert_true(IsHidden(group));
}

ZTEST(screen_group, test_activate_and_deactivate_are_idempotent) {
    auto root = MakeRoot();
    auto group = std::make_shared<ScreenGroup>(0, root);

    auto screen = std::make_shared<FakeScreen>(1, 0, 0, true, group->GetContainer());
    group->AddScreen(screen);

    zassert_equal(group->EnsureRendered(), 0);

    group->Activate();
    group->Activate();
    zassert_equal(screen->activated_count, 1);

    group->Deactivate();
    group->Deactivate();
    zassert_equal(screen->deactivated_count, 1);
    zassert_true(IsHidden(group));

    group->Activate();
    zassert_equal(screen->activated_count, 2);
}

ZTEST(screen_group, test_group_lifecycle_reaches_invisible_screens_without_revealing_them) {
    auto root = MakeRoot();
    auto group = std::make_shared<ScreenGroup>(0, root);

    auto visible = std::make_shared<FakeScreen>(1, 0, 0, true, group->GetContainer());
    auto hidden = std::make_shared<FakeScreen>(2, 0, 0, false, group->GetContainer());
    group->AddScreen(visible);
    group->AddScreen(hidden);

    zassert_equal(group->EnsureRendered(), 0);
    group->Activate();

    zassert_equal(hidden->render_count, 1);
    zassert_equal(hidden->activated_count, 1);
    zassert_false(hidden->IsVisible());
    zassert_equal(visible->activated_count, 1);

    // Deactivation is unconditional: a screen made invisible while active still
    // has to be told to stand down.
    group->Deactivate();
    zassert_equal(hidden->deactivated_count, 1);
}

ZTEST(screen_group, test_get_screen_finds_screens_by_id) {
    auto root = MakeRoot();
    auto group = std::make_shared<ScreenGroup>(0, root);

    auto screen = std::make_shared<FakeScreen>(7, 0, 0, true, group->GetContainer());
    group->AddScreen(screen);
    group->AddScreen(nullptr);

    zassert_false(group->IsEmpty());
    zassert_equal(group->GetScreen(7).get(), screen.get());
    zassert_is_null(group->GetScreen(8).get());
}

// Sorting the vector alone changes nothing on screen: LVGL draws in child order.
ZTEST(screen_group, test_screens_are_drawn_in_z_index_order) {
    auto root = MakeRoot();
    auto group = std::make_shared<ScreenGroup>(0, root);

    auto top = std::make_shared<FakeScreen>(1, 0, 5, true, group->GetContainer());
    auto bottom = std::make_shared<FakeScreen>(2, 0, -5, true, group->GetContainer());
    auto middle = std::make_shared<FakeScreen>(3, 0, 0, true, group->GetContainer());

    group->AddScreen(top);
    group->AddScreen(bottom);
    group->AddScreen(middle);

    zassert_equal(group->EnsureRendered(), 0);

    auto* container = group->GetContainer()->GetObject();

    zassert_equal(lv_obj_get_index(bottom->GetContainer()->GetObject()), 0);
    zassert_equal(lv_obj_get_index(middle->GetContainer()->GetObject()), 1);
    zassert_equal(lv_obj_get_index(top->GetContainer()->GetObject()), 2);
    zassert_equal(lv_obj_get_child_count(container), 3);
}
