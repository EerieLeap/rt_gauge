#include <algorithm>

#include <zephyr/logging/log.h>
#include <lvgl.h>

#include "domain/ui_domain/lvgl_lock.h"

#include "screen_group.h"

namespace eerie_leap::views::screens {

using eerie_leap::domain::ui_domain::ScopedLvglLock;

LOG_MODULE_REGISTER(screen_group_logger);

ScreenGroup::ScreenGroup(uint32_t screen_group_id, std::shared_ptr<Frame> parent)
    : screen_group_id_(screen_group_id) {

    container_ = std::make_shared<Frame>(Frame::CreateWrapped(parent->GetObject())
        .SetProcessingParent(parent)
        .SetWidth(100, false)
        .SetHeight(100, false)
        .Build());

    lv_obj_add_flag(container_->GetObject(), LV_OBJ_FLAG_HIDDEN);
}

uint32_t ScreenGroup::GetGroupId() const {
    return screen_group_id_;
}

void ScreenGroup::AddScreen(std::shared_ptr<IScreen> screen) {
    if(screen == nullptr)
        return;

    auto insert_position = std::upper_bound(
        screens_.begin(),
        screens_.end(),
        screen->GetZIndex(),
        [](int32_t z_index, const std::shared_ptr<IScreen>& current) { return z_index < current->GetZIndex(); });

    screens_.insert(insert_position, std::move(screen));
}

std::shared_ptr<IScreen> ScreenGroup::GetScreen(uint32_t screen_id) const {
    for(const auto& screen : screens_)
        if(screen->GetId() == screen_id)
            return screen;

    return nullptr;
}

bool ScreenGroup::IsEmpty() const {
    return screens_.empty();
}

int ScreenGroup::EnsureRendered() {
    if(is_rendered_)
        return 0;

    int res = Render();
    if(res != 0) {
        LOG_ERR("Failed to render screen group %u.", screen_group_id_);
        return res;
    }

    is_rendered_ = true;
    ApplyActivation();

    return 0;
}

bool ScreenGroup::IsRendered() const {
    return is_rendered_;
}

// Recorded here and applied by EnsureRendered() when the group is rendered
// later, so a caller can select a group without waiting for it to be built.
void ScreenGroup::Activate() {
    ScopedLvglLock lvgl_guard;
    is_activation_requested_ = true;

    ApplyActivation();
}

void ScreenGroup::ApplyActivation() {
    ScopedLvglLock lvgl_guard;
    if(is_activated_ || !is_activation_requested_ || !is_rendered_)
        return;

    lv_obj_remove_flag(container_->GetObject(), LV_OBJ_FLAG_HIDDEN);

    for(auto& screen : screens_)
        screen->OnActivated();

    container_->Invalidate();
    is_activated_ = true;
}

void ScreenGroup::Deactivate() {
    ScopedLvglLock lvgl_guard;
    is_activation_requested_ = false;

    if(!is_activated_)
        return;

    for(auto& screen : screens_)
        screen->OnDeactivated();

    lv_obj_add_flag(container_->GetObject(), LV_OBJ_FLAG_HIDDEN);
    is_activated_ = false;
}

bool ScreenGroup::IsActivated() const {
    return is_activated_;
}

// screens_ is ordered by ascending z-index and LVGL draws the last child on top,
// so the vector order has to be pushed into the container's child order.
void ScreenGroup::ApplyScreenOrder() {
    int32_t index = 0;

    for(auto& screen : screens_) {
        auto screen_container = screen->GetContainer();
        if(screen_container == nullptr || screen_container->GetObject() == nullptr)
            continue;

        lv_obj_move_to_index(screen_container->GetObject(), index++);
    }
}

int ScreenGroup::DoRender() {
    for(auto& screen : screens_) {
        int res = screen->Render();
        if(res != 0) {
            LOG_ERR("Failed to render screen %u in group %u.", screen->GetId(), screen_group_id_);
            return res;
        }
    }

    ApplyScreenOrder();

    return 0;
}

int ScreenGroup::ApplyTheme(const ITheme& theme) {
    return 0;
}

} // namespace eerie_leap::views::screens
