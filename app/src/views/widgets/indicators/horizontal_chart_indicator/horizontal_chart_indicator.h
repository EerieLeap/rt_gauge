#pragma once

#include <lvgl.h>

#include "views/widgets/indicators/indicator_base.h"

namespace eerie_leap::views::widgets::indicators {

enum class HorizontalChartIndicatorType : uint8_t {
    None = 0,
    Bar,
    Line,
};

// A chart of the last CHART_POINT_COUNT values that moves left as values arrive. It sits in a viewport, one point
// wider than the box with the extra point hidden left of it: with IS_SMOOTHED the chart scrolls the width of a point
// back to rest over the interval between values, so a new point slides in instead of the whole line jumping a slot.
class HorizontalChartIndicator : public IndicatorBase {
private:
    // A longer gap between values is a stall, not a rate to scroll over.
    static constexpr uint32_t MAX_SCROLL_INTERVAL_MS = 10000;
    static constexpr int32_t MAX_POINT_COUNT = 1000;

    int32_t point_count_{};
    HorizontalChartIndicatorType chart_type_{};
    lv_obj_t* viewport_{};
    lv_obj_t* lv_chart_{};

    // What one new point moves the others, in pixels.
    float step_px_ = 0;
    int32_t translate_px_ = 0;
    bool has_last_point_ = false;
    uint32_t last_point_ms_ = 0;
    // When the value was received, so a replay on resume or the first render is stamped with the event's time, not
    // its own: the next scroll then spans the real gap between events, not the gap to the replay.
    bool has_received_ms_ = false;
    uint32_t received_ms_ = 0;
    bool has_interval_ = false;
    uint32_t interval_ms_ = 0;

    int32_t PointCount() const;
    void UpdateIndicator(float value) override;
    lv_obj_t* Create(lv_obj_t* parent, HorizontalChartIndicatorType type);
    void UpdateGeometry();
    void SetTranslate(int32_t pixels);
    void CompleteScroll();
    void StartScroll(uint32_t duration_ms);
    uint32_t UpdateInterval(uint32_t elapsed_ms);

    static void ScrollCallback(void* obj, int32_t pixels);
    static void ViewportCallback(lv_event_t* event);

    int DoRender() override;
    int ApplyTheme(const ITheme& theme) override;

public:
    explicit HorizontalChartIndicator(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context);
    ~HorizontalChartIndicator() override;
    HorizontalChartIndicator(const HorizontalChartIndicator&) = delete;
    HorizontalChartIndicator& operator=(const HorizontalChartIndicator&) = delete;
    HorizontalChartIndicator(HorizontalChartIndicator&&) = delete;
    HorizontalChartIndicator& operator=(HorizontalChartIndicator&&) = delete;

    WidgetType GetType() const override {
        return WidgetType::IndicatorHorizontalChart;
    }

    // A chart appends one sample per call. IndicatorBase::Update()'s eased interpolation is meant for needles and
    // readouts: it would insert many interpolated samples per real one and distort the timebase.
    void Update(float value) override;

protected:
    void RegisterProperties(WidgetPropertyStore& store) const override;
    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override;
    void OnProcessingSuspended() override;
    void OnPropertyReceived(WidgetPropertyType type) override;
};

} // namespace eerie_leap::views::widgets::indicators
