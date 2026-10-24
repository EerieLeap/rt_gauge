#include <exception>
#include <utility>

#include <zephyr/logging/log.h>

#include "domain/ui_domain/lvgl_lock.h"

#include "ui_highlight.h"

namespace eerie_leap::views {

using eerie_leap::domain::ui_domain::ScopedLvglLock;

LOG_MODULE_REGISTER(ui_highlight_logger);

UiHighlight::UiHighlight(Resolver resolver) :
    resolver_(std::move(resolver)) {}

UiHighlight::~UiHighlight() {
    ScopedLvglLock lvgl_guard;

    if(timer_ != nullptr)
        lv_timer_delete(timer_);

    if(outline_ != nullptr)
        lv_obj_delete(outline_);
}

void UiHighlight::Set(std::optional<uint32_t> screen, std::optional<uint32_t> widget) {
    ScopedLvglLock lvgl_guard;

    if(outline_ == nullptr) {
        outline_ = lv_obj_create(lv_layer_top());

        // A frame and nothing else: a touch goes through to what it is over.
        lv_obj_set_clickable(outline_, false);
        lv_obj_set_scrollable(outline_, false);
        lv_obj_set_floating(outline_, true);
        lv_obj_set_style_bg_opa(outline_, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_pad_all(outline_, 0, LV_PART_MAIN);
        lv_obj_set_style_radius(outline_, 0, LV_PART_MAIN);
        // White inside a black line: it shows on a dark and on a light widget alike.
        lv_obj_set_style_border_width(outline_, BORDER_PX, LV_PART_MAIN);
        lv_obj_set_style_border_color(outline_, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_border_opa(outline_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_outline_width(outline_, 1, LV_PART_MAIN);
        lv_obj_set_style_outline_color(outline_, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_outline_opa(outline_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_hidden(outline_, true);
    }

    if(timer_ == nullptr)
        timer_ = lv_timer_create(FollowCb, FOLLOW_PERIOD_MS, this);

    screen_ = screen;
    widget_ = widget;
    is_set_ = true;

    Follow();
}

void UiHighlight::Clear() {
    ScopedLvglLock lvgl_guard;

    is_set_ = false;
    screen_.reset();
    widget_.reset();

    if(timer_ != nullptr) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }

    Hide();
}

bool UiHighlight::IsSet() const {
    ScopedLvglLock lvgl_guard;

    return is_set_;
}

bool UiHighlight::IsVisible() const {
    ScopedLvglLock lvgl_guard;

    return is_set_ && outline_ != nullptr && !lv_obj_is_hidden(outline_);
}

lv_obj_t* UiHighlight::GetObject() const {
    ScopedLvglLock lvgl_guard;

    return outline_;
}

// Invoked by LVGL on the renderer thread, which already holds the LVGL lock. An exception escaping back into
// LVGL's C code would skip that unlock.
void UiHighlight::FollowCb(lv_timer_t* timer) {
    auto* highlight = static_cast<UiHighlight*>(lv_timer_get_user_data(timer));
    if(highlight == nullptr)
        return;

    try {
        highlight->Follow();
    } catch(const std::exception& e) {
        LOG_ERR("Failed to follow the highlighted widget. %s", e.what());
    } catch(...) {
        LOG_ERR("Failed to follow the highlighted widget.");
    }
}

void UiHighlight::Follow() {
    if(!is_set_ || outline_ == nullptr)
        return;

    const lv_obj_t* target = resolver_ ? resolver_(screen_, widget_) : nullptr;
    if(target == nullptr) {
        Hide();

        return;
    }

    // The screens may have changed since the target was last laid out.
    lv_obj_update_layout(target);

    lv_area_t area;
    lv_obj_get_coords(target, &area);

    lv_obj_set_pos(outline_, area.x1 - BORDER_PX, area.y1 - BORDER_PX);
    lv_obj_set_size(outline_, lv_area_get_width(&area) + 2 * BORDER_PX, lv_area_get_height(&area) + 2 * BORDER_PX);
    lv_obj_set_hidden(outline_, false);

    // Above an overlay that was pushed after it.
    const lv_obj_t* layer = lv_obj_get_parent(outline_);
    if(lv_obj_get_index(outline_) != static_cast<int32_t>(lv_obj_get_child_count(layer)) - 1)
        lv_obj_move_foreground(outline_);
}

void UiHighlight::Hide() {
    if(outline_ != nullptr)
        lv_obj_set_hidden(outline_, true);
}

} // namespace eerie_leap::views
