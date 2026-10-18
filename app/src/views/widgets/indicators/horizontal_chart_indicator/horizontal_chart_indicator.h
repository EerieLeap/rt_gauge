#pragma once

#include <lvgl.h>

#include "views/widgets/indicators/indicator_base.h"

namespace eerie_leap::views::widgets::indicators {

enum class HorizontalChartIndicatorType : uint8_t {
    None = 0,
    Bar,
    Line,
};

class HorizontalChartIndicator : public IndicatorBase {
private:
    int32_t point_count_;
    HorizontalChartIndicatorType chart_type_;
    lv_obj_t* lv_chart_;

    void UpdateIndicator(float value) override;
    lv_obj_t* Create(
        lv_obj_t* parent,
        int32_t range_start,
        int32_t range_end,
        int32_t point_count,
        HorizontalChartIndicatorType type
    );

    int DoRender() override;
    int ApplyTheme(const ITheme& theme) override;

public:
    explicit HorizontalChartIndicator(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context);
    WidgetType GetType() const override {
        return WidgetType::IndicatorHorizontalChart;
    }

    // Charts advance their series by one discrete sample per call (SHIFT
    // mode). IndicatorBase::Update()'s animated interpolation is meant for
    // needles/digital readouts; routing a chart through it would insert
    // many interpolated "fake" samples per real sensor sample, distorting
    // the chart's timebase and reshifting/redrawing the whole series on
    // every animation frame instead of once per real update.
    void Update(float value) override;

protected:
    void RegisterProperties(WidgetPropertyStore& store) const override;
    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override;

public:
};

} // namespace eerie_leap::views::widgets::indicators
