#include <cmath>

#include "domain/ui_domain/models/widget_property.h"

#include "views/widgets/indicators/indicator_base.h"
#include "views/themes/theme_manager.h"

#include "segment_arc_indicator.h"

namespace eerie_leap::views::widgets::indicators {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::utilitites;
using eerie_leap::views::utilities::LvglColor;

SegmentArcIndicator::SegmentArcIndicator(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context)
    : IndicatorBase(id, std::move(parent), std::move(context)) { }

SegmentArcIndicator::~SegmentArcIndicator() {
    DetachDispatch();

    segments_.clear();
}

int SegmentArcIndicator::DoRender() {
    Create(container_->GetObject(), static_cast<int32_t>(range_start_), static_cast<int32_t>(range_end_));

    return 0;
}

void SegmentArcIndicator::RegisterProperties(WidgetPropertyStore& store) const {
    IndicatorBase::RegisterProperties(store);

    store.RegisterColor(WidgetPropertyType::COLOR_PRIMARY_ACTIVE);
    store.RegisterColor(WidgetPropertyType::COLOR_PRIMARY_INACTIVE);

    store.Register(WidgetPropertyType::START_ANGLE, ConfigValue { 45 }, PropertyChangeEffect::Repaint);
    store.Register(WidgetPropertyType::END_ANGLE, ConfigValue { 315 }, PropertyChangeEffect::Repaint);
}

int SegmentArcIndicator::ApplyTheme(const ITheme& theme) {
    const auto active = properties_->ResolveColor(WidgetPropertyType::COLOR_PRIMARY_ACTIVE, theme.GetSecondaryColor());
    const auto inactive = properties_->ResolveColor(WidgetPropertyType::COLOR_PRIMARY_INACTIVE, LvglColor(0, 0));

    for(auto& segment : segments_) {
        const auto& color = segment.is_active ? active : inactive;
        lv_obj_set_style_arc_color(segment.lv_segment, color.ToLvColor(), LV_PART_INDICATOR | LV_STATE_DEFAULT);
        lv_obj_set_style_arc_opa(segment.lv_segment, color.ToLvOpa(), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    }

    return 0;
}

lv_obj_t* SegmentArcIndicator::CreateSegment(lv_obj_t* parent, int start_angle, int end_angle, int i) {
    int seg_start_angle = start_angle + 90 + i * step_deg_;
    int seg_end_angle = start_angle + 90 + i * step_deg_ + 3;

    if(seg_end_angle > end_angle + 90)
        return nullptr;

    auto lv_arc_segment = lv_arc_create(parent);

    lv_arc_set_range(lv_arc_segment, 0, 1);
    lv_obj_set_width(lv_arc_segment, lv_pct(100));
    lv_obj_set_height(lv_arc_segment, lv_pct(100));
    lv_obj_set_align(lv_arc_segment, LV_ALIGN_CENTER);
    lv_obj_remove_flag(lv_arc_segment, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_value(lv_arc_segment, 1);
    lv_obj_set_style_arc_width(lv_arc_segment, segment_length_, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_rounded(lv_arc_segment, false, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    lv_arc_set_bg_angles(lv_arc_segment, seg_start_angle, seg_end_angle);

    lv_obj_set_style_bg_opa(lv_arc_segment, 0, LV_PART_KNOB | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(lv_arc_segment, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_arc_opa(lv_arc_segment, 0, LV_PART_INDICATOR | LV_STATE_DEFAULT);

    return lv_arc_segment;
}

void SegmentArcIndicator::Create(lv_obj_t* parent, int32_t range_start, int32_t range_end) {
    segments_.clear();

    for(int i = 0; i < 1000; i++) {
        auto segment = CreateSegment(parent, start_angle_, end_angle_, i);
        if(segment == nullptr)
            break;

        segments_.push_back({
            .lv_segment = segment,
            .is_active = false
        });
    }

    segments_per_value_ = (range_end - range_start) / static_cast<float>(segments_.size());
}

void SegmentArcIndicator::UpdateIndicator(float value) {
    int segment_index = value / segments_per_value_;
    if(segment_index >= segments_.size())
        segment_index = segments_.size() - 1;

    for(size_t i = 0; i < segments_.size(); i++) {
        segments_[i].is_active = i <= segment_index;
    }

    ApplyTheme(ThemeManager::GetInstance().GetCurrentTheme());
}

void SegmentArcIndicator::OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) {
    switch(type) {
        case WidgetPropertyType::START_ANGLE:
            start_angle_ = ConfigValueAs<int>(value, 45);
            break;

        case WidgetPropertyType::END_ANGLE:
            end_angle_ = ConfigValueAs<int>(value, 315);
            break;

        default:
            IndicatorBase::OnPropertyChanged(type, value);
            break;
    }
}

} // namespace eerie_leap::views::widgets::indicators
