#pragma once

// #include "utilities/math/ema_filter.hpp"
#include "views/widgets/widget_base.h"

namespace eerie_leap::views::widgets::indicators {

// using EmaFilterInt32 = eerie_leap::utilities::math::EmaFilter<int32_t>;

class IndicatorBase : public WidgetBase {
private:
    static void UpdateIndicatorCallback(void* obj, int32_t value);

    // EmaFilterInt32 value_filter_;
    // uint8_t smoothing_factor_ = 4;

    // Sensor samples can arrive faster than the value-change animation
    // can settle (ValueChangeAnimation() below), so without a dead-zone
    // a near-flat/noisy signal keeps restarting the animation before it
    // ever finishes - the widget stays "in motion" and gets invalidated/
    // redrawn on every render tick indefinitely, even with no visually
    // meaningful change. Skip re-triggering for tiny deltas.
    static constexpr float MIN_ANIMATION_DELTA_RATIO = 0.001F; // 0.1% of range

protected:
    lv_anim_t value_change_animation_{};
    float range_start_ = 0;
    float range_end_ = 0;
    float value_ = 0;

    // IS_SMOOTHED eases between values shown one after another while the indicator processes. The first value after
    // the build or a pause is shown at once: there is nothing live to ease from, and a bound VALUE holds the record's
    // value until the first event, which is no reading.
    bool can_ease_ = false;
    bool has_received_value_ = false; // An inbound binding delivered VALUE since the build or the last pause.
    bool is_value_bound_ = false;

    // Whether the next value eases from the one on display (IS_SMOOTHED and a live value shown).
    bool ShouldEaseNextValue() const {
        return IsSmoothed() && can_ease_;
    }
    // A value was shown without easing. Easing resumes once what is shown came from the source: always for a value
    // without an inbound binding, else after the binding delivered one.
    void OnValueShownAtOnce() {
        can_ease_ = has_received_value_ || !is_value_bound_;
    }

    virtual void UpdateIndicator(float value) = 0;
    lv_anim_t CreateValueChangeAnimation();
    static void ValueChangeAnimation(lv_anim_t& anim, float range, float start_value, float end_value);

public:
    IndicatorBase(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context);
    ~IndicatorBase() override;
    IndicatorBase(const IndicatorBase&) = delete;
    IndicatorBase& operator=(const IndicatorBase&) = delete;
    IndicatorBase(IndicatorBase&&) = delete;
    IndicatorBase& operator=(IndicatorBase&&) = delete;

    virtual void Update(float value);

protected:
    void OnProcessingSuspended() override;
    void OnPropertyReceived(WidgetPropertyType type) override;
    void OnConfigured() override;
    void RegisterProperties(WidgetPropertyStore& store) const override;
    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override;
};

} // namespace eerie_leap::views::widgets::indicators
