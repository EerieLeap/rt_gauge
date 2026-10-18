#pragma once

#include <lvgl.h>

#include "views/widgets/indicators/indicator_base.h"

namespace eerie_leap::views::widgets::indicators {

class DigitalIndicator : public IndicatorBase {
private:
    lv_obj_t* lv_label_{};
    int value_precision_{};

    void UpdateIndicator(float value) override;
    lv_obj_t* Create(const std::shared_ptr<Frame>& parent);

    int DoRender() override;
    int ApplyTheme(const ITheme& theme) override;

protected:
    void RegisterProperties(WidgetPropertyStore& store) const override;
    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override;

public:
    explicit DigitalIndicator(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context);
    WidgetType GetType() const override {
        return WidgetType::IndicatorDigital;
    }
};

} // namespace eerie_leap::views::widgets::indicators
