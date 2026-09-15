#pragma once

#include <lvgl.h>

#include "domain/ui_domain/models/widget_direction.h"
#include "views/widgets/indicators/indicator_base.h"

namespace eerie_leap::views::widgets::indicators {

using eerie_leap::domain::ui_domain::models::WidgetDirection;

class BarIndicator : public IndicatorBase {
protected:
    lv_obj_t* lv_bar_;
    WidgetDirection direction_;

    void UpdateIndicator(float value) override;

    virtual lv_obj_t* Create(lv_obj_t* parent, int32_t range_start, int32_t range_end);

    int DoRender() override;
    int ApplyTheme(const ITheme& theme) override;

protected:
    void RegisterProperties(WidgetPropertyStore& store) const override;
    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override;

public:
    static void UpdateDirection(lv_obj_t* lv_bar, WidgetDirection direction, int32_t range_start, int32_t range_end);

public:
    explicit BarIndicator(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context);
    WidgetType GetType() const override { return WidgetType::IndicatorBar; }
};

} // namespace eerie_leap::views::widgets::indicators
