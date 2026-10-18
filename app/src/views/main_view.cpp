#include <algorithm>
#include <cerrno>
#include <string>
#include <utility>

#include <lvgl.h>
#include <zephyr/logging/log.h>

#include "domain/ui_domain/lvgl_lock.h"

#include "views/themes/theme_manager.h"

#include "main_view.h"

namespace eerie_leap::views {

using namespace eerie_leap::views::themes;

using eerie_leap::domain::ui_domain::ScopedLvglLock;

LOG_MODULE_REGISTER(main_view_logger);

MainView::MainView() {
    container_ = std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(100, false).SetHeight(100, false).Build());
}

std::shared_ptr<Frame> MainView::GetContainer() const {
    return container_;
}

void MainView::SetRenderDispatcher(RenderDispatcher dispatcher) {
    render_dispatcher_ = std::move(dispatcher);
}

std::shared_ptr<ScreenGroup> MainView::GetOrCreateGroup(uint32_t screen_group_id) {
    const auto it = screen_groups_.find(screen_group_id);
    if(it != screen_groups_.end())
        return it->second;

    auto screen_group = std::make_shared<ScreenGroup>(screen_group_id, container_);
    screen_groups_.emplace(screen_group_id, screen_group);

    return screen_group;
}

void MainView::AddScreen(std::shared_ptr<IScreen> screen) {
    if(screen == nullptr)
        return;

    GetOrCreateGroup(screen->GetGroupId())->AddScreen(std::move(screen));
}

std::shared_ptr<Frame> MainView::GetGroupContainer(uint32_t screen_group_id) {
    return GetOrCreateGroup(screen_group_id)->GetContainer();
}

void MainView::PruneEmptyGroups() {
    // GetGroupContainer() creates a group up front, so a screen that failed to
    // configure can leave behind a blank but navigable group.
    for(auto it = screen_groups_.begin(); it != screen_groups_.end();) {
        if(it->second->IsEmpty()) {
            LOG_WRN("Dropping screen group %u, it has no screens.", it->first);
            it = screen_groups_.erase(it);
        } else {
            ++it;
        }
    }
}

int MainView::SetActiveGroup(uint32_t screen_group_id) {
    const auto it = screen_groups_.find(screen_group_id);
    if(it == screen_groups_.end()) {
        LOG_WRN("Screen group %u is not configured.", screen_group_id);
        return -ENOENT;
    }

    // Before the view itself is rendered only record the selection; DoRender activates it.
    if(!IsReady()) {
        active_screen_group_id_ = screen_group_id;
        return 0;
    }

    if(active_screen_group_id_ == screen_group_id)
        return 0;

    std::shared_ptr<ScreenGroup> previous_screen_group = nullptr;
    if(active_screen_group_id_.has_value()) {
        const auto previous = screen_groups_.find(*active_screen_group_id_);
        if(previous != screen_groups_.end())
            previous_screen_group = previous->second;
    }

    if(previous_screen_group != nullptr)
        previous_screen_group->Deactivate();

    const auto& screen_group = it->second;

    // Recorded now, applied by the group as soon as it is rendered.
    screen_group->Activate();
    active_screen_group_id_ = screen_group_id;

    if(screen_group->IsRendered())
        return 0;

    if(render_dispatcher_ != nullptr) {
        // Capturing `this` is safe because the dispatcher's work queue is drained
        // and stopped before this view is destroyed.
        const auto deferred_screen_group = screen_group;
        render_dispatcher_([this, deferred_screen_group, previous_screen_group] {
            ScopedLvglLock lvgl_guard;

            RenderGroupOrRollBack(deferred_screen_group, previous_screen_group);
        });

        return 0;
    }

    return RenderGroupOrRollBack(screen_group, previous_screen_group);
}

int MainView::RenderGroupOrRollBack(
    const std::shared_ptr<ScreenGroup>& screen_group,
    const std::shared_ptr<ScreenGroup>& previous_screen_group
) {

    const int res = screen_group->EnsureRendered();
    if(res == 0)
        return 0;

    LOG_ERR("Failed to render screen group %u. Error: %d.", screen_group->GetGroupId(), res);

    // A later switch already moved on; that one owns the active state now.
    if(active_screen_group_id_ != screen_group->GetGroupId())
        return res;

    screen_group->Deactivate();

    active_screen_group_id_ = previous_screen_group != nullptr
        ? std::optional<uint32_t>(previous_screen_group->GetGroupId())
        : std::nullopt;

    if(previous_screen_group != nullptr)
        previous_screen_group->Activate();

    return res;
}

std::optional<uint32_t> MainView::GetActiveGroupId() const {
    return active_screen_group_id_;
}

std::vector<uint32_t> MainView::GetGroupIds() const {
    std::vector<uint32_t> screen_group_ids;
    screen_group_ids.reserve(screen_groups_.size());

    for(const auto& [screen_group_id, screen_group] : screen_groups_)
        screen_group_ids.push_back(screen_group_id);

    std::sort(screen_group_ids.begin(), screen_group_ids.end());

    return screen_group_ids;
}

std::shared_ptr<IScreen> MainView::GetScreen(uint32_t screen_id) const {
    for(const auto& [screen_group_id, screen_group] : screen_groups_) {
        auto screen = screen_group->GetScreen(screen_id);
        if(screen != nullptr)
            return screen;
    }

    return nullptr;
}

static void RenderCenterCrossHelperGuides(lv_obj_t* screen) {
    lv_obj_t* panel1 = lv_obj_create(screen);
    lv_obj_set_width(panel1, 2);
    lv_obj_set_height(panel1, 466);
    lv_obj_set_align(panel1, LV_ALIGN_CENTER);
    lv_obj_remove_flag(panel1, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_color(
        panel1, ThemeManager::GetInstance().GetCurrentTheme().GetPrimaryColor().ToLvColor(),
        LV_PART_MAIN | LV_STATE_DEFAULT
    );
    lv_obj_set_style_border_opa(panel1, 255, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_t* panel2 = lv_obj_create(screen);
    lv_obj_set_width(panel2, 466);
    lv_obj_set_height(panel2, 2);
    lv_obj_set_align(panel2, LV_ALIGN_CENTER);
    lv_obj_remove_flag(panel2, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_color(
        panel2, ThemeManager::GetInstance().GetCurrentTheme().GetPrimaryColor().ToLvColor(),
        LV_PART_MAIN | LV_STATE_DEFAULT
    );
    lv_obj_set_style_border_opa(panel2, 255, LV_PART_MAIN | LV_STATE_DEFAULT);
}

int MainView::DoRender() {
    if(!active_screen_group_id_.has_value()) {
        LOG_ERR("No active screen screen_group, nothing to render.");
        return -ENOENT;
    }

    const auto it = screen_groups_.find(*active_screen_group_id_);
    if(it == screen_groups_.end()) {
        LOG_ERR("Active screen group %u is gone.", *active_screen_group_id_);
        return -ENOENT;
    }

    const int res = it->second->EnsureRendered();
    if(res != 0)
        return res;

    it->second->Activate();

    // RenderCenterCrossHelperGuides(container_->GetObject());

    return 0;
}

int MainView::ApplyTheme(const ITheme& theme) {
    lv_obj_set_style_bg_color(
        container_->GetObject(), theme.GetBackgroundColor().ToLvColor(), LV_PART_MAIN | LV_STATE_DEFAULT
    );
    lv_obj_set_style_bg_opa(
        container_->GetObject(), theme.GetBackgroundColor().ToLvOpa(), LV_PART_MAIN | LV_STATE_DEFAULT
    );

    return 0;
}

} // namespace eerie_leap::views
