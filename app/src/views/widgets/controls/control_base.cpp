#include <exception>
#include <utility>

#include <zephyr/logging/log.h>

#include "control_base.h"

namespace eerie_leap::views::widgets::controls {

LOG_MODULE_REGISTER(control_base_logger);

ControlBase::ControlBase(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context, bool consumes_gestures)
    : SettingWidgetBase(id, std::move(parent), std::move(context)) {

    // Without this a drag over an interactive control would climb to the gesture
    // root and navigate away instead of moving the control.
    if(consumes_gestures)
        lv_obj_remove_flag(container_->GetObject(), LV_OBJ_FLAG_GESTURE_BUBBLE);
}

ControlBase::~ControlBase() {
    DetachDispatch();

    // Removes every code registered for this (callback, instance) pair.
    if(event_object_ != nullptr) {
        lv_obj_remove_event_cb_with_user_data(event_object_, EventCb, this);
        lv_obj_remove_event_cb_with_user_data(event_object_, InputGuardCb, this);
    }
}

void ControlBase::AttachEvents(lv_obj_t* object, std::initializer_list<lv_event_code_t> codes) {
    if(object == nullptr)
        return;

    // A stale registration would keep handing `this` to LVGL after destruction.
    if(event_object_ != nullptr && event_object_ != object) {
        lv_obj_remove_event_cb_with_user_data(event_object_, EventCb, this);
        lv_obj_remove_event_cb_with_user_data(event_object_, InputGuardCb, this);
    }

    if(event_object_ != object)
        lv_obj_add_event_cb(object, InputGuardCb, static_cast<lv_event_code_t>(LV_EVENT_ALL | LV_EVENT_PREPROCESS), this);

    event_object_ = object;

    for(auto code : codes)
        lv_obj_add_event_cb(object, EventCb, code, this);
}

// Invoked by LVGL on the renderer thread, which already holds the LVGL lock.
// An exception escaping back into LVGL's C code would skip that unlock.
void ControlBase::EventCb(lv_event_t* e) {
    ScopedLvglLock lvgl_guard;
    auto* control = static_cast<ControlBase*>(lv_event_get_user_data(e));
    if(control == nullptr || !control->IsAnimationEligible())
        return;

    try {
        control->OnControlEvent(lv_event_get_code(e));
    } catch(const std::exception& ex) {
        LOG_ERR("Failed to handle control event. %s", ex.what());
    } catch(...) {
        LOG_ERR("Failed to handle control event.");
    }
}

void ControlBase::OnControlEvent(lv_event_code_t) { }

void ControlBase::InputGuardCb(lv_event_t* event) {
    const auto code = lv_event_get_code(event);
    if(!((code >= LV_EVENT_PRESSED && code <= LV_EVENT_ROTARY) || code == LV_EVENT_VALUE_CHANGED))
        return;

    ScopedLvglLock lvgl_guard;
    auto* control = static_cast<ControlBase*>(lv_event_get_user_data(event));
    if(control == nullptr)
        return;
    if(control->IsAnimationEligible()) {
        control->ReplayPendingProperties();
        return;
    }

    lv_event_stop_processing(event);
    lv_event_stop_bubbling(event);
    control->OnProcessingSuspended();
}

void ControlBase::OnProcessingSuspended() {
    if(event_object_ == nullptr)
        return;

    lv_obj_remove_state(event_object_, LV_STATE_PRESSED);
    OnPropertyChanged(WidgetPropertyType::VALUE, properties_->Get(WidgetPropertyType::VALUE));
}

} // namespace eerie_leap::views::widgets::controls
