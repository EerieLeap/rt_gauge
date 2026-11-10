#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>
#include <utility>

#include "domain/ui_domain/models/widget_property.h"
#include "utilities/type/config_value.h"
#include "views/themes/theme_manager.h"

#include "horizontal_chart_indicator.h"

namespace eerie_leap::views::widgets::indicators {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::utilitites;
using namespace eerie_leap::views::themes;

HorizontalChartIndicator::HorizontalChartIndicator(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context) :
    IndicatorBase(id, std::move(parent), std::move(context)) {}

HorizontalChartIndicator::~HorizontalChartIndicator() {
    DetachDispatch();

    ScopedLvglLock lvgl_guard;
    // The animation and the viewport's callback target `this`. A viewport deleted earlier cleared the pointer.
    lv_anim_delete(this, ScrollCallback);
    if(viewport_ != nullptr)
        lv_obj_remove_event_cb_with_user_data(viewport_, ViewportCallback, this);
}

int32_t HorizontalChartIndicator::PointCount() const {
    return std::clamp<int32_t>(point_count_, 1, MAX_POINT_COUNT);
}

int HorizontalChartIndicator::DoRender() {
    // A rebuild starts an empty chart: nothing to scroll or to measure an interval from.
    lv_anim_delete(this, ScrollCallback);
    translate_px_ = 0;
    has_last_point_ = false;
    has_interval_ = false;

    auto* viewport = Create(content_frame_->GetObject(), chart_type_);
    const auto child = std::make_shared<Frame>(Frame::Create(viewport).Build());
    content_frame_->SetChild(child);

    lv_obj_update_layout(viewport_);
    UpdateGeometry();

    return 0;
}

void HorizontalChartIndicator::RegisterProperties(WidgetPropertyStore& store) const {
    IndicatorBase::RegisterProperties(store);

    store.RegisterColor(WidgetPropertyType::COLOR_PRIMARY_ACTIVE);

    store.Register(WidgetPropertyType::CHART_POINT_COUNT, ConfigValue{ 30 }, PropertyChangeEffect::Rebuild);
    store.Register(
        WidgetPropertyType::CHART_TYPE, ConfigValue{ static_cast<int>(HorizontalChartIndicatorType::Bar) },
        PropertyChangeEffect::Rebuild
    );
}

int HorizontalChartIndicator::ApplyTheme(const ITheme& theme) {
    const auto primary = properties_->ResolveColor(WidgetPropertyType::COLOR_PRIMARY_ACTIVE, theme.GetPrimaryColor());

    auto* ser = lv_chart_get_series_next(lv_chart_, nullptr);
    lv_chart_set_series_color(lv_chart_, ser, primary.ToLvColor());
    // LVGL's bar renderer forces its rectangles to full opacity. Composite this style-free,
    // single-series chart once for both chart types; the outer wrapper owns widget opacity.
    lv_obj_set_style_line_opa(lv_chart_, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(lv_chart_, LV_OPA_COVER, LV_PART_ITEMS);
    lv_obj_set_style_opa_layered(lv_chart_, primary.ToLvOpa(), LV_PART_MAIN);

    return 0;
}

lv_obj_t* HorizontalChartIndicator::Create(lv_obj_t* parent, HorizontalChartIndicatorType type) {
    lv_chart_type_t chart_type = LV_CHART_TYPE_NONE;
    switch(type) {
        case HorizontalChartIndicatorType::Bar:
            chart_type = LV_CHART_TYPE_BAR;
            break;
        case HorizontalChartIndicatorType::Line:
            chart_type = LV_CHART_TYPE_LINE;
            break;
        case HorizontalChartIndicatorType::None:
            break;
    }

    if(chart_type == LV_CHART_TYPE_NONE)
        throw std::runtime_error("Invalid chart type");

    // The viewport clips the chart, which is wider than the box by the point hidden on its left.
    viewport_ = lv_obj_create(parent);
    lv_obj_remove_style_all(viewport_);
    lv_obj_set_size(viewport_, lv_pct(100), lv_pct(100));
    lv_obj_set_scrollable(viewport_, false);
    lv_obj_set_clickable(viewport_, false);
    lv_obj_add_event_cb(viewport_, ViewportCallback, LV_EVENT_SIZE_CHANGED, this);
    lv_obj_add_event_cb(viewport_, ViewportCallback, LV_EVENT_DELETE, this);

    lv_chart_ = lv_chart_create(viewport_);
    lv_obj_remove_style_all(lv_chart_);
    lv_obj_set_scrollable(lv_chart_, false);
    lv_obj_set_clickable(lv_chart_, false);

    if(type == HorizontalChartIndicatorType::Bar)
        lv_obj_set_style_pad_column(lv_chart_, 2, LV_PART_MAIN);
    else
        lv_obj_set_style_line_width(lv_chart_, 6, LV_PART_ITEMS);

    lv_chart_set_type(lv_chart_, chart_type);

    lv_chart_set_update_mode(lv_chart_, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_size(lv_chart_, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_height(lv_chart_, lv_pct(100));

    // Values sit on the fixed scale of IndicatorBase, so the chart moves in sub-unit steps whatever the range is.
    lv_chart_set_range(lv_chart_, LV_CHART_AXIS_PRIMARY_Y, 0, LVGL_VALUE_SCALE);

    lv_chart_set_point_count(lv_chart_, static_cast<uint32_t>(PointCount() + 1));
    auto* ser = lv_chart_add_series(lv_chart_, lv_color_hex(0x000000), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_series_color(lv_chart_, ser, lv_color_hex(0x3D3D3D));

    lv_obj_set_style_line_opa(lv_chart_, LV_OPA_COVER, LV_PART_ITEMS);

    return viewport_;
}

// At rest the visible part of the chart is exactly a chart of N points over the box. The chart has one point more,
// to the left of the box, and is that much wider; a new point shifts the others by one step.
void HorizontalChartIndicator::UpdateGeometry() {
    if(viewport_ == nullptr || lv_chart_ == nullptr)
        return;

    CompleteScroll();

    const int32_t count = PointCount();
    const int32_t width = lv_obj_get_content_width(viewport_);
    const bool is_line = chart_type_ == HorizontalChartIndicatorType::Line;
    // A line's points sit on the box's edges, so the hidden one is a gap away; a bar's are slots of an equal width.
    const int32_t slots = is_line ? std::max(count - 1, 1) : count;
    const int32_t extra = std::lround(static_cast<float>(width) / static_cast<float>(slots));
    const int32_t chart_width = width + extra;

    lv_obj_set_width(lv_chart_, chart_width);
    lv_obj_set_pos(lv_chart_, -extra, 0);
    step_px_ = static_cast<float>(chart_width) / static_cast<float>(is_line ? count : count + 1);
}

void HorizontalChartIndicator::SetTranslate(int32_t pixels) {
    if(pixels == translate_px_ || lv_chart_ == nullptr)
        return;

    translate_px_ = pixels;
    lv_obj_set_style_translate_x(lv_chart_, pixels, LV_PART_MAIN);
}

void HorizontalChartIndicator::CompleteScroll() {
    lv_anim_delete(this, ScrollCallback);
    SetTranslate(0);
}

void HorizontalChartIndicator::StartScroll(uint32_t duration_ms) {
    const auto step = static_cast<int32_t>(std::lround(step_px_));
    if(step <= 0)
        return;

    // The shift moved every point one step left: translate back so they stay where they were drawn, then slide.
    SetTranslate(step);

    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, this);
    lv_anim_set_exec_cb(&anim, ScrollCallback);
    lv_anim_set_values(&anim, step, 0);
    lv_anim_set_duration(&anim, duration_ms);
    lv_anim_set_path_cb(&anim, lv_anim_path_linear);
    lv_anim_start(&anim);
}

void HorizontalChartIndicator::ScrollCallback(void* obj, int32_t pixels) {
    ScopedLvglLock lvgl_guard;
    auto* chart = static_cast<HorizontalChartIndicator*>(obj);
    if(!chart->IsAnimationEligible()) {
        chart->OnProcessingSuspended();
        return;
    }

    chart->SetTranslate(pixels);
}

void HorizontalChartIndicator::ViewportCallback(lv_event_t* event) {
    auto* chart = static_cast<HorizontalChartIndicator*>(lv_event_get_user_data(event));
    if(lv_event_get_target(event) != chart->viewport_)
        return;

    if(lv_event_get_code(event) == LV_EVENT_SIZE_CHANGED) {
        chart->UpdateGeometry();

        return;
    }

    // The viewport is going away, with the chart in it.
    lv_anim_delete(chart, ScrollCallback);
    chart->viewport_ = nullptr;
    chart->lv_chart_ = nullptr;
}

// The first interval is taken as is, the next ones move the estimate a quarter of the way.
uint32_t HorizontalChartIndicator::UpdateInterval(uint32_t elapsed_ms) {
    interval_ms_ = has_interval_ ? (3 * interval_ms_ + elapsed_ms) / 4 : elapsed_ms;
    interval_ms_ = std::clamp<uint32_t>(interval_ms_, 1, MAX_SCROLL_INTERVAL_MS);
    has_interval_ = true;

    return interval_ms_;
}

void HorizontalChartIndicator::UpdateIndicator(float value) {
    auto* ser = lv_chart_get_series_next(lv_chart_, nullptr);

    lv_chart_set_next_value(lv_chart_, ser, ToLvglScale(value));
}

void HorizontalChartIndicator::Update(float value) {
    ScopedLvglLock lvgl_guard;
    if(!IsAnimationEligible() || lv_chart_ == nullptr)
        return;

    if(value < range_start_)
        value = range_start_;
    else if(value > range_end_)
        value = range_end_;

    const uint32_t now = lv_tick_get();
    const uint32_t elapsed = has_last_point_ ? lv_tick_elaps(last_point_ms_) : 0;
    const bool is_scrolling = ShouldEaseNextValue() && has_last_point_ && elapsed <= MAX_SCROLL_INTERVAL_MS;

    // An early value cuts the scroll short; what is left of it is small with a steady source.
    CompleteScroll();
    UpdateIndicator(value);
    value_ = value;
    last_point_ms_ = now;
    has_last_point_ = true;

    if(is_scrolling) {
        StartScroll(UpdateInterval(elapsed));

        return;
    }

    // A stall is no rate to scroll over, nor to estimate from.
    has_interval_ = false;
    OnValueShownAtOnce();
}

void HorizontalChartIndicator::OnProcessingSuspended() {
    CompleteScroll();
    has_last_point_ = false;
    has_interval_ = false;

    IndicatorBase::OnProcessingSuspended();
}

void HorizontalChartIndicator::OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) {
    switch(type) {
        case WidgetPropertyType::CHART_POINT_COUNT:
            point_count_ = ConfigValueAs<int>(value, 30);
            break;

        case WidgetPropertyType::CHART_TYPE:
            chart_type_ = static_cast<HorizontalChartIndicatorType>(
                ConfigValueAs<int>(value, static_cast<int>(HorizontalChartIndicatorType::Bar))
            );
            break;

        default:
            IndicatorBase::OnPropertyChanged(type, value);
            break;
    }
}

} // namespace eerie_leap::views::widgets::indicators
