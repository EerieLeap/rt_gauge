#include <cmath>

#include "domain/ui_domain/models/widget_property.h"

#include "indicator_base.h"

namespace eerie_leap::views::widgets::indicators {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;

IndicatorBase::IndicatorBase(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context)
    : WidgetBase(id, std::move(parent), std::move(context)) { // , value_filter_(0) {

    value_change_animation_ = CreateValueChangeAnimation();
}

IndicatorBase::~IndicatorBase() {
    DetachDispatch();

    // The animation targets `this`, so it must not outlive the indicator.
    lv_anim_delete(this, UpdateIndicatorCallback);
}

void IndicatorBase::UpdateIndicatorCallback(void* obj, int32_t value) {
    ScopedLvglLock lvgl_guard;
    auto* indicator = static_cast<IndicatorBase*>(obj);
    if(!indicator->IsAnimationEligible()) {
        indicator->OnProcessingSuspended();
        return;
    }

    float value_float = static_cast<float>(value) / (10 * CONFIG_EERIE_LEAP_FLOAT_SIGNIFICANT_DIGITS);
    indicator->UpdateIndicator(value_float);
    indicator->value_ = value_float;
}

lv_anim_t IndicatorBase::CreateValueChangeAnimation() {
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, this);
    lv_anim_set_exec_cb(&anim, UpdateIndicatorCallback);
    lv_anim_set_repeat_count(&anim, 0);

    return anim;
}

void IndicatorBase::ValueChangeAnimation(lv_anim_t& anim, float range, float start_value, float end_value) {
    uint32_t duration = 4000;
    uint32_t unit_duration = duration / range;
    uint32_t change_duration = unit_duration * abs(end_value - start_value);

    lv_anim_set_duration(&anim, change_duration);
    lv_anim_set_values(&anim,
        static_cast<int32_t>(start_value * 10 * CONFIG_EERIE_LEAP_FLOAT_SIGNIFICANT_DIGITS),
        static_cast<int32_t>(end_value * 10 * CONFIG_EERIE_LEAP_FLOAT_SIGNIFICANT_DIGITS));
    lv_anim_start(&anim);
}

// void IndicatorBase::Update(float value) {
//     if(!IsReady())
//         return;

//     int32_t scaled_value = static_cast<int32_t>(
//         value * 10 * CONFIG_EERIE_LEAP_FLOAT_SIGNIFICANT_DIGITS);

//     if(IsSmoothed()) {
//         int32_t filtered = value_filter_.Filter(scaled_value, smoothing_factor_);
//         UpdateIndicatorCallback(this, filtered);
//     } else {
//         UpdateIndicatorCallback(this, scaled_value);
//     }
// }

// NOTE: Actual LVGL animation renders frames smoother
// compared to Exponential Moving Average Filter which
// can be jumpy and result in noticeable tearing effect
void IndicatorBase::Update(float value) {
    ScopedLvglLock lvgl_guard;
    if(!IsAnimationEligible())
        return;

    if(value < range_start_)
        value = range_start_;
    else if(value > range_end_)
        value = range_end_;

    if(IsSmoothed()) {
        if(std::abs(value - value_) < min_animation_delta_ratio_ * (range_end_ - range_start_))
            return;

        ValueChangeAnimation(
            value_change_animation_,
            range_end_ - range_start_,
            value_,
            value);
    } else {
        UpdateIndicatorCallback(this, static_cast<int32_t>(value * 10 * CONFIG_EERIE_LEAP_FLOAT_SIGNIFICANT_DIGITS));
    }
}

void IndicatorBase::OnProcessingSuspended() {
    // The store already contains the target, but the display has not reached it. Retain that
    // work even if no further value event arrives before processing becomes eligible again.
    if(lv_anim_delete(this, UpdateIndicatorCallback))
        pending_properties_.set(static_cast<size_t>(WidgetPropertyType::VALUE));
}

void IndicatorBase::RegisterProperties(WidgetPropertyStore& store) const {
    WidgetBase::RegisterProperties(store);

    store.Register(WidgetPropertyType::MIN_VALUE, ConfigValue { 0.0 }, PropertyChangeEffect::Repaint);
    store.Register(WidgetPropertyType::MAX_VALUE, ConfigValue { 100.0 }, PropertyChangeEffect::Repaint);

    // No effect: Update() drives the animation, which invalidates just the area it moves. Adding
    // a container repaint here dirties the whole widget rect on every sample instead.
    store.Register(WidgetPropertyType::VALUE, ConfigValue { 0.0 }, PropertyChangeEffect::None);
}

void IndicatorBase::OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) {
    switch(type) {
        case WidgetPropertyType::MIN_VALUE:
            range_start_ = ConfigValueAs<double>(value, 0);
            break;

        case WidgetPropertyType::MAX_VALUE:
            range_end_ = ConfigValueAs<double>(value, 100);
            break;

        case WidgetPropertyType::VALUE:
            Update(static_cast<float>(ConfigValueAs<double>(value, 0)));
            break;

        default:
            WidgetBase::OnPropertyChanged(type, value);
            break;
    }
}

} // namespace eerie_leap::views::widgets::indicators
