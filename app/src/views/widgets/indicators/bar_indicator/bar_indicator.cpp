#include <utility>

#include <zephyr/kernel.h>

#include "domain/ui_domain/models/widget_property.h"

#include "views/widgets/indicators/indicator_base.h"
#include "views/themes/theme_manager.h"

#include "bar_indicator.h"

namespace eerie_leap::views::widgets::indicators {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::utilitites;
using eerie_leap::views::utilities::LvglColor;

BarIndicator::BarIndicator(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context)
    : IndicatorBase(id, std::move(parent), std::move(context)) {}

int BarIndicator::DoRender() {
    auto lv_obj = Create(container_->GetObject(), static_cast<int32_t>(range_start_), static_cast<int32_t>(range_end_));
    auto child = std::make_shared<Frame>(
        Frame::Create(lv_obj).Build());
    container_->SetChild(child);

    return 0;
}

void BarIndicator::RegisterProperties(WidgetPropertyStore& store) const {
    IndicatorBase::RegisterProperties(store);

    store.RegisterColor(WidgetPropertyType::COLOR_PRIMARY_ACTIVE);
    store.RegisterColor(WidgetPropertyType::COLOR_SECONDARY_ACTIVE);

    store.Register(
        WidgetPropertyType::DIRECTION,
        ConfigValue { static_cast<int>(WidgetDirection::LeftToRight) },
        PropertyChangeEffect::Repaint);
}

int BarIndicator::ApplyTheme(const ITheme& theme) {
    const auto primary = properties_->ResolveColor(WidgetPropertyType::COLOR_PRIMARY_ACTIVE, theme.GetSecondaryColor());
    const auto secondary = properties_->ResolveColor(WidgetPropertyType::COLOR_SECONDARY_ACTIVE, LvglColor(0, 0));

    lv_obj_set_style_bg_color(lv_bar_, primary.ToLvColor(), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(lv_bar_, primary.ToLvOpa(), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(lv_bar_, secondary.ToLvColor(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(lv_bar_, secondary.ToLvOpa(), LV_PART_MAIN | LV_STATE_DEFAULT);

    return 0;
}

lv_obj_t* BarIndicator::Create(lv_obj_t* parent, int32_t range_start, int32_t range_end) {
    lv_bar_ = lv_bar_create(parent);
    lv_obj_remove_style_all(lv_bar_);

    UpdateDirection(lv_bar_, direction_, range_start, range_end);

    lv_obj_set_width(lv_bar_, lv_pct(100));
    lv_obj_set_height(lv_bar_, lv_pct(100));
    lv_obj_set_align(lv_bar_, LV_ALIGN_CENTER);
    lv_obj_remove_flag(lv_bar_, LV_OBJ_FLAG_CLICKABLE);
    lv_bar_set_value(lv_bar_, range_start, LV_ANIM_OFF);

    // Background color - LV_PART_MAIN
    // Bar color - LV_PART_INDICATOR
    lv_obj_set_style_bg_opa(lv_bar_, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    return lv_bar_;
}

void BarIndicator::UpdateDirection(lv_obj_t* lv_bar, WidgetDirection direction, int32_t range_start, int32_t range_end) {
    if(direction == WidgetDirection::LeftToRight || direction == WidgetDirection::RightToLeft)
        lv_bar_set_orientation(lv_bar, LV_BAR_ORIENTATION_HORIZONTAL);
    else
        lv_bar_set_orientation(lv_bar, LV_BAR_ORIENTATION_VERTICAL);

    lv_bar_set_range(lv_bar, range_start, range_end);

    if(direction == WidgetDirection::RightToLeft)
        lv_obj_set_style_base_dir(lv_bar, LV_BASE_DIR_RTL, 0);
    else if(direction == WidgetDirection::TopToBottom)
        lv_bar_set_range(lv_bar, range_end, range_start);
}

void BarIndicator::UpdateIndicator(float value) {
    lv_bar_set_value(lv_bar_, static_cast<int32_t>(value), LV_ANIM_OFF);
}

void BarIndicator::OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) {
    if(type == WidgetPropertyType::DIRECTION) {
        direction_ = static_cast<WidgetDirection>(
            ConfigValueAs<int>(value, static_cast<int>(WidgetDirection::LeftToRight)));

        return;
    }

    IndicatorBase::OnPropertyChanged(type, value);
}

} // namespace eerie_leap::views::widgets::indicators
