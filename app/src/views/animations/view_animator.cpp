#include <algorithm>

#include <zephyr/logging/log.h>

#include "view_animator.h"

namespace eerie_leap::views::animations {

LOG_MODULE_REGISTER(view_animator_logger);

ViewAnimator::~ViewAnimator() {
    Detach();
}

bool ViewAnimator::Attach(utilitites::Frame& presentation, utilitites::Frame& layout,
    EligibilityCallback eligible, void* context) {
    auto* presentation_object = presentation.GetObject();
    auto* layout_object = layout.GetObject();
    if(presentation_object == nullptr || layout_object == nullptr || eligible == nullptr
        || lv_obj_get_parent(presentation_object) != layout_object)
        return false;

    Detach();
    presentation_ = presentation_object;
    layout_ = layout_object;
    eligibility_ = eligible;
    context_ = context;
    lv_obj_add_event_cb(presentation_, ObjectEvent, LV_EVENT_ALL, this);
    lv_obj_add_event_cb(layout_, ObjectEvent, LV_EVENT_ALL, this);
    ResetPresentation();
    return true;
}

void ViewAnimator::Synchronize(const Settings& settings) {
    if(presentation_ == nullptr || layout_ == nullptr)
        return;
    if(settings.duration_ms < Animation::MIN_DURATION_MS || settings.duration_ms > Animation::MAX_DURATION_MS
        || (settings.type != Animation::Type::None && settings.type != Animation::Type::Blinking
            && settings.type != Animation::Type::Rotation))
        return;

    const bool eligible = eligibility_(context_);
    if(synchronized_ && settings_ == settings && eligible_ == eligible)
        return;

    Cancel();
    ResetPresentation();
    settings_ = settings;
    eligible_ = eligible;
    synchronized_ = true;
    if(eligible_ && settings_.active && settings_.type != Animation::Type::None)
        Start();
}

void ViewAnimator::Start() {
    if(settings_.type == Animation::Type::Rotation) {
        layout_overflow_ = lv_obj_has_flag(layout_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        presentation_overflow_ = lv_obj_has_flag(presentation_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        rotation_prepared_ = true;
        lv_obj_add_flag(layout_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_add_flag(presentation_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    }

    lv_anim_t animation;
    lv_anim_init(&animation);
    lv_anim_set_var(&animation, this);
    lv_anim_set_user_data(&animation, this);
    lv_anim_set_exec_cb(&animation, Execute);
    lv_anim_set_deleted_cb(&animation, Deleted);
    lv_anim_set_repeat_count(&animation, LV_ANIM_REPEAT_INFINITE);
    if(settings_.type == Animation::Type::Blinking) {
        const auto forward = settings_.duration_ms / 2;
        lv_anim_set_duration(&animation, forward);
        lv_anim_set_reverse_duration(&animation, settings_.duration_ms - forward);
        lv_anim_set_values(&animation, LV_OPA_COVER, LV_OPA_TRANSP);
        lv_anim_set_path_cb(&animation, lv_anim_path_ease_in_out);
    } else {
        lv_anim_set_duration(&animation, settings_.duration_ms);
        lv_anim_set_values(&animation, 0, 3600);
        lv_anim_set_path_cb(&animation, lv_anim_path_linear);
    }

    running_ = true;
    starting_ = true;
    cancel_after_start_ = false;
    const auto* started = lv_anim_start(&animation);
    starting_ = false;
    if(started == nullptr) {
        running_ = false;
        ResetPresentation();
        LOG_ERR("Failed to allocate view animation.");
    } else if(cancel_after_start_) {
        Cancel();
        ResetPresentation();
    }
}

void ViewAnimator::Cancel() {
    if(starting_) {
        cancel_after_start_ = true;
        return;
    }
    lv_anim_delete(this, Execute);
    running_ = false;
}

void ViewAnimator::ResetPresentation() {
    if(presentation_ != nullptr) {
        lv_obj_set_style_opa_layered(presentation_, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_transform_rotation(presentation_, 0, LV_PART_MAIN);
        if(rotation_prepared_)
            lv_obj_set_flag(presentation_, LV_OBJ_FLAG_OVERFLOW_VISIBLE, presentation_overflow_);
    }
    if(rotation_prepared_ && layout_ != nullptr)
        lv_obj_set_flag(layout_, LV_OBJ_FLAG_OVERFLOW_VISIBLE, layout_overflow_);
    rotation_prepared_ = false;
    RefreshDrawMargin();
}

void ViewAnimator::RefreshDrawMargin() {
    if(layout_ != nullptr)
        lv_obj_refresh_ext_draw_size(layout_);
}

void ViewAnimator::StopAndReset() {
    Cancel();
    ResetPresentation();
    synchronized_ = false;
}

void ViewAnimator::Detach() {
    StopAndReset();
    if(presentation_ != nullptr)
        lv_obj_remove_event_cb_with_user_data(presentation_, ObjectEvent, this);
    if(layout_ != nullptr)
        lv_obj_remove_event_cb_with_user_data(layout_, ObjectEvent, this);
    presentation_ = nullptr;
    layout_ = nullptr;
    eligibility_ = nullptr;
    context_ = nullptr;
}

bool ViewAnimator::IsRunning() const {
    return running_;
}

void ViewAnimator::Execute(void* context, int32_t value) {
    auto* self = static_cast<ViewAnimator*>(context);
    if(self->presentation_ == nullptr || self->eligibility_ == nullptr || !self->eligibility_(self->context_)) {
        self->eligible_ = false;
        self->Cancel();
        self->ResetPresentation();
        return;
    }
    if(self->settings_.type == Animation::Type::Blinking)
        lv_obj_set_style_opa_layered(self->presentation_, value, LV_PART_MAIN);
    else {
        lv_obj_set_style_transform_rotation(self->presentation_, value, LV_PART_MAIN);
        self->RefreshDrawMargin();
    }
}

void ViewAnimator::Deleted(lv_anim_t* animation) {
    auto* self = static_cast<ViewAnimator*>(lv_anim_get_user_data(animation));
    self->running_ = false;
}

void ViewAnimator::OnObjectDeleted(lv_obj_t* object) {
    if(object == layout_) {
        layout_ = nullptr;
        if(presentation_ != nullptr)
            lv_obj_remove_event_cb_with_user_data(presentation_, ObjectEvent, this);
        presentation_ = nullptr;
        rotation_prepared_ = false;
    } else {
        presentation_ = nullptr;
    }
    Cancel();
    ResetPresentation();
    if(layout_ != nullptr)
        lv_obj_remove_event_cb_with_user_data(layout_, ObjectEvent, this);
    layout_ = nullptr;
    eligibility_ = nullptr;
    context_ = nullptr;
    synchronized_ = false;
}

void ViewAnimator::ObjectEvent(lv_event_t* event) {
    auto* self = static_cast<ViewAnimator*>(lv_event_get_user_data(event));
    auto* object = lv_event_get_target_obj(event);
    const auto code = lv_event_get_code(event);
    if(object != self->presentation_ && object != self->layout_)
        return;
    if(code == LV_EVENT_DELETE) {
        self->OnObjectDeleted(object);
    } else if(code == LV_EVENT_SIZE_CHANGED && self->rotation_prepared_) {
        self->RefreshDrawMargin();
    } else if(code == LV_EVENT_REFR_EXT_DRAW_SIZE && object == self->layout_
        && self->rotation_prepared_ && self->presentation_ != nullptr) {
        lv_area_t bounds;
        lv_area_t layout;
        lv_obj_get_coords(self->presentation_, &bounds);
        lv_obj_get_transformed_area(self->presentation_, &bounds, LV_OBJ_POINT_TRANSFORM_FLAG_NONE);
        lv_obj_get_coords(self->layout_, &layout);
        const auto margin = std::max<int32_t>({ 0, layout.x1 - bounds.x1, layout.y1 - bounds.y1,
            bounds.x2 - layout.x2, bounds.y2 - layout.y2 });
        lv_event_set_ext_draw_size(event, margin + 1);
    }
}

} // namespace eerie_leap::views::animations
