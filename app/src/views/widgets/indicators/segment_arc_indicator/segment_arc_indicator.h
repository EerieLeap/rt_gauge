#pragma once

#include <lvgl.h>

#include "views/widgets/indicators/indicator_base.h"

namespace eerie_leap::views::widgets::indicators {

// NOTE: Current implementation is not optimal and requires too much memory
class SegmentArcIndicator : public IndicatorBase {
private:
    struct Segment {
        lv_obj_t* lv_segment;
        bool is_active;
    };

    static const int segment_length_ = 46;
    static const int step_deg_ = 4;

    int start_angle_;
    int end_angle_;
    float segments_per_value_;

    std::vector<Segment> segments_;

    void UpdateIndicator(float value) override;

    static lv_obj_t* CreateSegment(lv_obj_t* parent, int start_angle, int end_angle, int i);
    void Create(lv_obj_t* parent, int32_t range_start, int32_t range_end);

    int DoRender() override;
    int ApplyTheme(const ITheme& theme) override;

protected:
    void RegisterProperties(WidgetPropertyStore& store) const override;
    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override;

public:
public:
    ~SegmentArcIndicator() override;

    explicit SegmentArcIndicator(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context);
    WidgetType GetType() const override {
        return WidgetType::IndicatorSegmentArc;
    }
};

} // namespace eerie_leap::views::widgets::indicators
