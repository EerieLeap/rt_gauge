#include <cerrno>
#include <exception>
#include <stdexcept>
#include <utility>

#include <lvgl.h>
#include <zephyr/logging/log.h>

#include "domain/ui_domain/lvgl_lock.h"

#include "views/themes/theme_manager.h"

#include "overlay_host.h"

namespace eerie_leap::views {

using namespace eerie_leap::views::themes;

using eerie_leap::domain::ui_domain::ScopedLvglLock;

LOG_MODULE_REGISTER(overlay_host_logger);

OverlayHost::Entry::~Entry() {
    // First: LVGL runs it from the renderer thread and it would reach a host that
    // no longer has this overlay.
    if(auto_close_timer != nullptr)
        lv_timer_delete(auto_close_timer);

    // Teardown also runs on the push failure path, so it may not throw its way out.
    if(screen_group != nullptr) {
        try {
            screen_group->Deactivate();
        } catch(const std::exception& e) {
            LOG_ERR("Failed to deactivate an overlay group. %s", e.what());
        } catch(...) {
            LOG_ERR("Failed to deactivate an overlay group.");
        }
    }

    // screen_group and scrim follow, each taking its LVGL objects with it.
}

OverlayHost::OverlayHost(std::shared_ptr<NavigationService> navigation_service) :
    navigation_service_(std::move(navigation_service)) {

    lv_obj_t* top_layer = lv_layer_top();
    if(top_layer == nullptr)
        throw std::runtime_error("No display to host overlays on");

    container_ = std::make_shared<Frame>(
        Frame::CreateWrapped(top_layer).SetWidth(100, false).SetHeight(100, false).Build()
    );

    lv_obj_set_clickable(container_->GetObject(), false);
    lv_obj_set_hidden(container_->GetObject(), true);
}

// Unlike the rest of the class this locks for itself: teardown does not come
// from the navigation path that already holds the lock.
OverlayHost::~OverlayHost() {
    ScopedLvglLock lvgl_guard;

    DismissAll();
}

std::unique_ptr<ScreenGroup> OverlayHost::CreateGroup(uint32_t screen_group_id) {
    return std::make_unique<ScreenGroup>(screen_group_id, container_);
}

int OverlayHost::Push(std::unique_ptr<ScreenGroup> screen_group, const OverlayOptions& options) {
    if(screen_group == nullptr || screen_group->IsEmpty())
        return -EINVAL;

    if(entries_.size() >= MAX_DEPTH) {
        LOG_WRN("Refusing to stack more than %zu overlays.", MAX_DEPTH);
        return -ENOSPC;
    }

    auto entry = std::make_unique<Entry>();
    entry->screen_group = std::move(screen_group);

    int res = -EIO;

    // Rendering and activating both reach widget code that can throw. Either the
    // overlay ends up on the stack or ~Entry takes all of it back.
    try {
        res = Mount(*entry, options);
    } catch(const std::exception& e) {
        LOG_ERR("Failed to push an overlay. %s", e.what());
    } catch(...) {
        LOG_ERR("Failed to push an overlay.");
    }

    if(res != 0)
        return res;

    entries_.push_back(std::move(entry));
    UpdateVisibility();

    return 0;
}

int OverlayHost::Mount(Entry& entry, const OverlayOptions& options) {
    if(options.is_modal)
        entry.scrim = CreateScrim(options.close_on_scrim_tap);

    const int res = entry.screen_group->EnsureRendered();
    if(res != 0) {
        LOG_ERR("Failed to render overlay group %u. Error: %d.", entry.screen_group->GetGroupId(), res);

        return res;
    }

    // The scrim was created after the group's container, so it would otherwise
    // cover the overlay it is supposed to sit behind.
    if(const auto container = entry.screen_group->GetContainer())
        lv_obj_move_foreground(container->GetObject());

    entry.screen_group->Activate();

    if(options.auto_close_ms > 0) {
        entry.auto_close_timer = lv_timer_create(AutoCloseCb, options.auto_close_ms, this);
        if(entry.auto_close_timer != nullptr)
            lv_timer_set_repeat_count(entry.auto_close_timer, 1);
    }

    return 0;
}

int OverlayHost::Pop() {
    if(entries_.empty())
        return -ENOENT;

    entries_.pop_back();
    UpdateVisibility();

    return 0;
}

void OverlayHost::DismissAll() {
    while(Pop() == 0) {
    }
}

bool OverlayHost::IsEmpty() const {
    return entries_.empty();
}

size_t OverlayHost::GetDepth() const {
    return entries_.size();
}

ScreenGroup* OverlayHost::GetTopGroup() const {
    return entries_.empty() ? nullptr : entries_.back()->screen_group.get();
}

std::shared_ptr<Frame> OverlayHost::CreateScrim(bool close_on_tap) {
    auto scrim = std::make_shared<Frame>(
        Frame::CreateWrapped(container_->GetObject()).SetWidth(100, false).SetHeight(100, false).Build()
    );

    // What makes the overlay modal, rather than a decoration the user can reach past.
    lv_obj_set_clickable(scrim->GetObject(), true);
    StyleScrim(scrim->GetObject(), ThemeManager::GetInstance().GetCurrentTheme());

    if(close_on_tap)
        lv_obj_add_event_cb(scrim->GetObject(), ScrimClickedCb, LV_EVENT_CLICKED, this);

    return scrim;
}

void OverlayHost::StyleScrim(lv_obj_t* scrim, const ITheme& theme) {
    lv_obj_set_style_bg_color(scrim, theme.GetBackgroundColor().ToLvColor(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scrim, SCRIM_OPACITY, LV_PART_MAIN);
}

void OverlayHost::UpdateVisibility() {
    if(container_ == nullptr || container_->GetObject() == nullptr)
        return;

    if(entries_.empty())
        lv_obj_set_hidden(container_->GetObject(), true);
    else
        lv_obj_set_hidden(container_->GetObject(), false);
}

// Invoked by LVGL on the renderer thread, which already holds the LVGL lock.
// An exception escaping back into LVGL's C code would skip that unlock.
void OverlayHost::ScrimClickedCb(lv_event_t* e) {
    auto* host = static_cast<OverlayHost*>(lv_event_get_user_data(e));
    if(host == nullptr)
        return;

    try {
        host->OnScrimClicked(lv_event_get_current_target_obj(e));
    } catch(const std::exception& ex) {
        LOG_ERR("Failed to handle an overlay scrim tap. %s", ex.what());
    } catch(...) {
        LOG_ERR("Failed to handle an overlay scrim tap.");
    }
}

void OverlayHost::AutoCloseCb(lv_timer_t* timer) {
    auto* host = static_cast<OverlayHost*>(lv_timer_get_user_data(timer));
    if(host == nullptr)
        return;

    try {
        host->OnAutoCloseElapsed(timer);
    } catch(const std::exception& ex) {
        LOG_ERR("Failed to auto-close an overlay. %s", ex.what());
    } catch(...) {
        LOG_ERR("Failed to auto-close an overlay.");
    }
}

void OverlayHost::OnScrimClicked(const lv_obj_t* scrim) {
    // A lower scrim is covered by the one above it, so only the top one can be
    // reached - and only the top one is what closing removes.
    if(entries_.empty())
        return;

    const auto& top = entries_.back();
    if(top->scrim == nullptr || top->scrim->GetObject() != scrim)
        return;

    RequestClose();
}

void OverlayHost::OnAutoCloseElapsed(const lv_timer_t* timer) {
    // A single-shot timer is deleted by LVGL right after this returns, so the
    // entry must stop naming it either way.
    for(auto& entry : entries_) {
        if(entry->auto_close_timer != timer)
            continue;

        entry->auto_close_timer = nullptr;

        // Something was stacked on top in the meantime; that one owns the close now.
        if(entry == entries_.back())
            RequestClose();

        return;
    }
}

void OverlayHost::RequestClose() {
    if(navigation_service_ == nullptr) {
        LOG_WRN("An overlay asked to close with no navigation service to route it.");
        return;
    }

    // Routed through navigation so the overlay state it guards is cleared too;
    // it publishes, and the resulting close action comes back here as Pop().
    navigation_service_->CloseOverlay();
}

int OverlayHost::DoRender() {
    // Overlays build themselves as they are pushed; there is nothing standing.
    return 0;
}

int OverlayHost::ApplyTheme(const ITheme& theme) {
    for(auto& entry : entries_)
        if(entry->scrim != nullptr)
            StyleScrim(entry->scrim->GetObject(), theme);

    return 0;
}

} // namespace eerie_leap::views
