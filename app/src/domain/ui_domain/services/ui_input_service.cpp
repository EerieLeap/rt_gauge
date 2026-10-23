#include <cerrno>
#include <exception>
#include <optional>
#include <utility>

#include <zephyr/logging/log.h>

#include "ui_input_service.h"

namespace eerie_leap::domain::ui_domain::services {

LOG_MODULE_REGISTER(ui_input_service_logger);

UiInputService::UiInputService(std::shared_ptr<NavigationService> navigation_service) :
    navigation_service_(std::move(navigation_service)) {

    SetGestureMapping(LV_DIR_LEFT, NavigationIntent::NextGroup);
    SetGestureMapping(LV_DIR_RIGHT, NavigationIntent::PreviousGroup);
}

UiInputService::~UiInputService() {
    if(root_ != nullptr)
        lv_obj_remove_event_cb_with_user_data(root_, GestureCb, this);
}

std::optional<size_t> UiInputService::ToIndex(lv_dir_t direction) {
    switch(direction) {
        case LV_DIR_LEFT:
            return 0;
        case LV_DIR_RIGHT:
            return 1;
        case LV_DIR_TOP:
            return 2;
        case LV_DIR_BOTTOM:
            return 3;
        default:
            return std::nullopt;
    }
}

int UiInputService::Initialize(lv_obj_t* root) {
    if(root == nullptr || navigation_service_ == nullptr)
        return -EINVAL;

    root_ = root;

    // lv_obj_create() sets GESTURE_BUBBLE on every child; LVGL walks up while that
    // flag is set, so the gesture target must be the first object without it.
    lv_obj_set_gesture_bubble(root_, false);
    lv_obj_add_event_cb(root_, GestureCb, LV_EVENT_GESTURE, this);

    LOG_INF("UI input service initialized.");

    return 0;
}

void UiInputService::SetGestureMapping(lv_dir_t direction, NavigationIntent intent, uint32_t argument) {
    if(auto index = ToIndex(direction)) {
        gesture_argument_[*index].store(argument, std::memory_order_relaxed);
        gesture_map_[*index].store(intent, std::memory_order_relaxed);
    }
}

// Invoked by LVGL on the renderer thread, which already holds the LVGL lock.
// An exception escaping back into LVGL's C code would skip that unlock.
void UiInputService::GestureCb(lv_event_t* e) {
    auto* service = static_cast<UiInputService*>(lv_event_get_user_data(e));
    if(service == nullptr)
        return;

    try {
        service->HandleGesture();
    } catch(const std::exception& ex) {
        LOG_ERR("Failed to handle gesture. %s", ex.what());
    } catch(...) {
        LOG_ERR("Failed to handle gesture.");
    }
}

void UiInputService::SetNavigationEnabled(bool is_enabled) {
    is_navigation_enabled_.store(is_enabled, std::memory_order_relaxed);
}

bool UiInputService::IsNavigationEnabled() const {
    return is_navigation_enabled_.load(std::memory_order_relaxed);
}

void UiInputService::HandleGesture() {
    lv_indev_t* indev = lv_indev_active();
    if(indev == nullptr)
        return;

    if(!is_navigation_enabled_.load(std::memory_order_relaxed))
        return;

    auto index = ToIndex(lv_indev_get_gesture_dir(indev));
    if(!index.has_value())
        return;

    const auto intent = gesture_map_[*index].load(std::memory_order_relaxed);
    if(intent == NavigationIntent::None)
        return;

    // One swipe must produce one action, not one per event tick.
    lv_indev_wait_release(indev);

    if(last_gesture_tick_ != 0 && lv_tick_elaps(last_gesture_tick_) < GESTURE_COOLDOWN_MS)
        return;

    last_gesture_tick_ = lv_tick_get();

    navigation_service_->Handle(intent, gesture_argument_[*index].load(std::memory_order_relaxed));
}

} // namespace eerie_leap::domain::ui_domain::services
