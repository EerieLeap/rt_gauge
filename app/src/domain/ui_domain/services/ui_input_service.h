#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include <lvgl.h>

#include "domain/ui_domain/models/navigation_intent.h"
#include "domain/ui_domain/services/navigation_service.h"

namespace eerie_leap::domain::ui_domain::services {

using eerie_leap::domain::ui_domain::models::NavigationIntent;

// Translates LVGL pointer gestures into navigation intents.
class UiInputService {
private:
    static constexpr size_t DIRECTION_COUNT = 4;
    static constexpr uint32_t GESTURE_COOLDOWN_MS = 250;

    std::shared_ptr<NavigationService> navigation_service_;

    // Read on the LVGL renderer thread, written by whoever configures the mapping.
    std::array<std::atomic<NavigationIntent>, DIRECTION_COUNT> gesture_map_{};

    // Only ShowOverlay/GoToGroup need one; written together with the intent above,
    // before the gesture callback is ever attached.
    std::array<std::atomic<uint32_t>, DIRECTION_COUNT> gesture_argument_{};

    // Off while a UI preview pins the screen: the gestures then do nothing, and the controls stay live.
    std::atomic<bool> is_navigation_enabled_ = true;

    // Every object the gestures are taken on.
    std::vector<lv_obj_t*> roots_;
    uint32_t last_gesture_tick_ = 0;

    static std::optional<size_t> ToIndex(lv_dir_t direction);
    static void GestureCb(lv_event_t* e);
    void HandleGesture();

public:
    explicit UiInputService(std::shared_ptr<NavigationService> navigation_service);
    ~UiInputService();

    UiInputService(const UiInputService&) = delete;
    UiInputService& operator=(const UiInputService&) = delete;

    int Initialize(lv_obj_t* root);

    /**
     * @brief Takes the gestures on @p root too. LVGL sends a gesture to the nearest object, from the pressed one up
     *        through its parents, that does not bubble it, so a layer apart from the first root's, such as the
     *        overlays' top layer, needs a root of its own.
     */
    int AddRoot(lv_obj_t* root);

    void SetGestureMapping(lv_dir_t direction, NavigationIntent intent, uint32_t argument = 0);

    /**
     * @brief Lets the gestures navigate, or not. Any thread. Only the gestures: a button on a screen still runs its
     *        own navigation intent.
     */
    void SetNavigationEnabled(bool is_enabled);
    [[nodiscard]] bool IsNavigationEnabled() const;
};

} // namespace eerie_leap::domain::ui_domain::services
