#pragma once

#include <lvgl.h>

#include "views/widgets/basic/icon_widget/icon_widget.h"
#include "views/widgets/indicators/indicator_base.h"

namespace eerie_leap::views::widgets::indicators {

using eerie_leap::views::widgets::basic::IconWidget;

class DialIndicator : public IndicatorBase {
private:
    int start_angle_;
    int end_angle_;

    static constexpr int DEFAULT_START_ANGLE = 45;
    static constexpr int DEFAULT_END_ANGLE = 315;

    lv_obj_t* lv_needle_icon_;
    std::unique_ptr<IconWidget> needle_icon_;

    void UpdateIndicator(float value) override;

    lv_obj_t* Create();

    int DoRender() override;
    int ApplyTheme(const ITheme& theme) override;

protected:
    void RegisterProperties(WidgetPropertyStore& store) const override;
    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override;

public:

    uint32_t GetAngleForValue(float value);

public:
    explicit DialIndicator(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context);
    ~DialIndicator() override;

    [[nodiscard]] WidgetType GetType() const override { return WidgetType::IndicatorDial; }
};

} // namespace eerie_leap::views::widgets::indicators
