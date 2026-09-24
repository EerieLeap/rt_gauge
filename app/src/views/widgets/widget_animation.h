#pragma once

#include <optional>

#include "views/animations/view_animator.h"
#include "views/utilitites/frame.h"
#include "views/widgets/widget_property_store.h"

namespace eerie_leap::views::widgets {

class WidgetAnimation {
public:
    using EligibilityCallback = animations::ViewAnimator::EligibilityCallback;

    bool Attach(utilitites::Frame& presentation, utilitites::Frame& layout,
        EligibilityCallback eligible, void* context);
    static void RegisterProperties(WidgetPropertyStore& store);
    bool ApplyProperty(WidgetPropertyType type, const ConfigValue& value);
    void SetRotation(int32_t angle);
    void Synchronize();
    void StopAndReset();
    void Detach();

private:
    std::optional<animations::ViewAnimator> animator_ { std::in_place };
    animations::ViewAnimator::Settings settings_;
    EligibilityCallback eligibility_ = nullptr;
    void* context_ = nullptr;
};

} // namespace eerie_leap::views::widgets
