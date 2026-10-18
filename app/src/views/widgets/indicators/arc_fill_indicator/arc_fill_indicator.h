#pragma once

#include <lvgl.h>

#include "views/widgets/indicators/indicator_base.h"

namespace eerie_leap::views::widgets::indicators {

class ArcFillIndicator : public IndicatorBase {
private:
    int start_angle_{};
    int end_angle_{};

    static constexpr int DEFAULT_START_ANGLE = 45;
    static constexpr int DEFAULT_END_ANGLE = 315;

    lv_obj_t* lv_arc_{};

    void UpdateIndicator(float value) override;

    lv_obj_t* Create(lv_obj_t* parent, int32_t range_start, int32_t range_end);

    int DoRender() override;
    int ApplyTheme(const ITheme& theme) override;

protected:
    void RegisterProperties(WidgetPropertyStore& store) const override;
    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override;

public:
    explicit ArcFillIndicator(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context);
    WidgetType GetType() const override {
        return WidgetType::IndicatorArcFill;
    }
};

} // namespace eerie_leap::views::widgets::indicators
