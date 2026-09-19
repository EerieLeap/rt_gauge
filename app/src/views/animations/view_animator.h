#pragma once

#include <lvgl.h>

#include "domain/ui_domain/models/animation.h"

namespace eerie_leap::views::animations {

class ViewAnimator {
public:
    using Animation = eerie_leap::domain::ui_domain::models::Animation;
    using EligibilityCallback = bool (*)(void* context);

    struct Settings {
        Animation::Type type = Animation::DEFAULT_TYPE;
        bool active = Animation::DEFAULT_ACTIVE;
        int duration_ms = Animation::DEFAULT_DURATION_MS;

        bool operator==(const Settings&) const = default;
    };

    ViewAnimator() = default;
    ~ViewAnimator();
    ViewAnimator(const ViewAnimator&) = delete;
    ViewAnimator& operator=(const ViewAnimator&) = delete;
    ViewAnimator(ViewAnimator&&) = delete;
    ViewAnimator& operator=(ViewAnimator&&) = delete;

    bool Attach(lv_obj_t* presentation, lv_obj_t* layout, EligibilityCallback eligible, void* context);
    void Synchronize(const Settings& settings);
    void StopAndReset();
    void Detach();
    bool IsRunning() const;

private:
    lv_obj_t* presentation_ = nullptr;
    lv_obj_t* layout_ = nullptr;
    EligibilityCallback eligibility_ = nullptr;
    void* context_ = nullptr;
    Settings settings_;
    bool synchronized_ = false;
    bool eligible_ = false;
    bool running_ = false;
    bool starting_ = false;
    bool cancel_after_start_ = false;
    bool rotation_prepared_ = false;
    bool layout_overflow_ = false;
    bool presentation_overflow_ = false;

    void Start();
    void Cancel();
    void ResetPresentation();
    void RefreshDrawMargin();
    void OnObjectDeleted(lv_obj_t* object);
    static void Execute(void* context, int32_t value);
    static void Deleted(lv_anim_t* animation);
    static void ObjectEvent(lv_event_t* event);
};

} // namespace eerie_leap::views::animations
