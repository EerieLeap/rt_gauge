#include <stdexcept>
#include <string>
#include <utility>

#include "utilities/type/config_value.h"
#include "domain/ui_domain/models/widget_property.h"
#include "views/themes/theme_manager.h"

#include "horizontal_chart_indicator.h"

namespace eerie_leap::views::widgets::indicators {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::utilitites;
using namespace eerie_leap::views::themes;

HorizontalChartIndicator::HorizontalChartIndicator(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context)
    : IndicatorBase(id, std::move(parent), std::move(context)) { }

int HorizontalChartIndicator::DoRender() {
    auto chart = Create(container_->GetObject(), static_cast<int32_t>(range_start_), static_cast<int32_t>(range_end_), point_count_, chart_type_);
    auto child = std::make_shared<Frame>(
        Frame::Create(chart).Build());
    container_->SetChild(child);

    return 0;
}

void HorizontalChartIndicator::RegisterProperties(WidgetPropertyStore& store) const {
    IndicatorBase::RegisterProperties(store);

    store.RegisterColor(WidgetPropertyType::COLOR_PRIMARY_ACTIVE);

    store.Register(WidgetPropertyType::CHART_POINT_COUNT, ConfigValue { 30 }, PropertyChangeEffect::Rebuild);
    store.Register(
        WidgetPropertyType::CHART_TYPE,
        ConfigValue { static_cast<int>(HorizontalChartIndicatorType::Bar) },
        PropertyChangeEffect::Rebuild);
}

int HorizontalChartIndicator::ApplyTheme(const ITheme& theme) {
    const auto primary = properties_->ResolveColor(WidgetPropertyType::COLOR_PRIMARY_ACTIVE, theme.GetPrimaryColor());

    auto ser = lv_chart_get_series_next(lv_chart_, nullptr);
    lv_chart_set_series_color(lv_chart_, ser, primary.ToLvColor());
    // LVGL's bar renderer forces its rectangles to full opacity. Composite this style-free,
    // single-series chart once for both chart types; the outer wrapper owns widget opacity.
    lv_obj_set_style_line_opa(lv_chart_, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(lv_chart_, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_DEFAULT);
    lv_obj_set_style_opa_layered(lv_chart_, primary.ToLvOpa(), LV_PART_MAIN | LV_STATE_DEFAULT);

    return 0;
}

lv_obj_t* HorizontalChartIndicator::Create(lv_obj_t* parent, int32_t range_start, int32_t range_end, int32_t point_count, HorizontalChartIndicatorType type) {
    lv_chart_ = lv_chart_create(parent);
    lv_obj_remove_style_all(lv_chart_);

    lv_chart_type_t chart_type = LV_CHART_TYPE_NONE;
    switch(type) {
        case HorizontalChartIndicatorType::Bar:
            chart_type = LV_CHART_TYPE_BAR;
            lv_obj_set_style_pad_column(lv_chart_, 2, LV_PART_MAIN);
            break;
        case HorizontalChartIndicatorType::Line:
            chart_type = LV_CHART_TYPE_LINE;
            lv_obj_set_style_line_width(lv_chart_, 6, LV_PART_ITEMS);
            break;
    }

    if(chart_type == LV_CHART_TYPE_NONE)
        throw std::runtime_error("Invalid chart type");

    lv_chart_set_type(lv_chart_, chart_type);

    lv_chart_set_update_mode(lv_chart_, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_size(lv_chart_, 0, 0, LV_PART_INDICATOR);
    lv_obj_set_width(lv_chart_, lv_pct(100));
    lv_obj_set_height(lv_chart_, lv_pct(100));

    lv_chart_set_range(lv_chart_, LV_CHART_AXIS_PRIMARY_Y, range_start, range_end);

    lv_chart_set_point_count(lv_chart_, point_count);
    auto ser = lv_chart_add_series(lv_chart_, lv_color_hex(0x000000), LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_series_color(lv_chart_, ser, lv_color_hex(0x3D3D3D));

    lv_obj_set_style_line_opa(lv_chart_, LV_OPA_COVER, LV_PART_ITEMS | LV_STATE_DEFAULT);

    return lv_chart_;
}

void HorizontalChartIndicator::UpdateIndicator(float value) {
    auto ser = lv_chart_get_series_next(lv_chart_, nullptr);

    lv_chart_set_next_value(lv_chart_, ser, static_cast<int32_t>(value));
}

void HorizontalChartIndicator::Update(float value) {
    if(!IsReady())
        return;

    if(value < range_start_)
        value = range_start_;
    else if(value > range_end_)
        value = range_end_;

    UpdateIndicator(value);
}

void HorizontalChartIndicator::OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) {
    switch(type) {
        case WidgetPropertyType::CHART_POINT_COUNT:
            point_count_ = ConfigValueAs<int>(value, 30);
            break;

        case WidgetPropertyType::CHART_TYPE:
            chart_type_ = static_cast<HorizontalChartIndicatorType>(
                ConfigValueAs<int>(value, static_cast<int>(HorizontalChartIndicatorType::Bar)));
            break;

        default:
            IndicatorBase::OnPropertyChanged(type, value);
            break;
    }
}

} // namespace eerie_leap::views::widgets::indicators
