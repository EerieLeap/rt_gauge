#include "widget_animation.h"

namespace eerie_leap::views::widgets {

using eerie_leap::domain::ui_domain::models::Animation;

bool WidgetAnimation::Attach(utilitites::Frame& presentation, utilitites::Frame& layout,
    EligibilityCallback eligible, void* context) {
    if(eligible == nullptr)
        return false;
    if(!animator_)
        animator_.emplace();
    if(!animator_->Attach(presentation, layout, [](void* context) {
        auto* animation = static_cast<WidgetAnimation*>(context);
        return animation->eligibility_(animation->context_);
    }, this))
        return false;

    eligibility_ = eligible;
    context_ = context;

    return true;
}

void WidgetAnimation::RegisterProperties(WidgetPropertyStore& store) {
    store.Register(WidgetPropertyType::ANIMATION_TYPE,
        static_cast<int>(Animation::DEFAULT_TYPE), PropertyChangeEffect::None);
    store.Register(WidgetPropertyType::IS_ANIMATION_ACTIVE, Animation::DEFAULT_ACTIVE, PropertyChangeEffect::None);
    store.Register(WidgetPropertyType::ANIMATION_DURATION_MS, Animation::DEFAULT_DURATION_MS, PropertyChangeEffect::None);
}

bool WidgetAnimation::ApplyProperty(WidgetPropertyType type, const ConfigValue& value) {
    switch(type) {
        case WidgetPropertyType::ANIMATION_TYPE:
            settings_.type = static_cast<Animation::Type>(std::get<int>(value));
            break;
        case WidgetPropertyType::IS_ANIMATION_ACTIVE:
            settings_.active = std::get<bool>(value);
            break;
        case WidgetPropertyType::ANIMATION_DURATION_MS:
            settings_.duration_ms = std::get<int>(value);
            break;
        default:
            return false;
    }

    return true;
}

bool WidgetAnimation::IsRotation() const {
    return settings_.type == Animation::Type::Rotation;
}

void WidgetAnimation::SetRotation(int32_t angle) {
    if(animator_)
        animator_->SetRotation(angle);
}

void WidgetAnimation::Synchronize() {
    if(animator_)
        animator_->Synchronize(settings_);
}

void WidgetAnimation::StopAndReset() {
    if(animator_)
        animator_->StopAndReset();
}

void WidgetAnimation::Detach() {
    animator_.reset();
    eligibility_ = nullptr;
    context_ = nullptr;
}

} // namespace eerie_leap::views::widgets
