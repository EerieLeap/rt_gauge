#include "domain/ui_domain/models/widget_property.h"

#include "views/themes/theme_manager.h"

#include "digital_indicator.h"

namespace eerie_leap::views::widgets::indicators {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::utilitites;
using namespace eerie_leap::views::themes;

DigitalIndicator::DigitalIndicator(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context)
    : IndicatorBase(id, std::move(parent), std::move(context)) {}

int DigitalIndicator::DoRender() {
    auto lv_obj = Create(container_);
    auto child = std::make_shared<Frame>(
        Frame::Create(lv_obj).Build());
    container_->SetChild(child);

    return 0;
}

void DigitalIndicator::RegisterProperties(WidgetPropertyStore& store) const {
    IndicatorBase::RegisterProperties(store);

    store.RegisterColor(WidgetPropertyType::COLOR_PRIMARY_ACTIVE);

    store.Register(WidgetPropertyType::VALUE_PRECISION, ConfigValue { 0 }, PropertyChangeEffect::Repaint);
}

int DigitalIndicator::ApplyTheme(const ITheme& theme) {
    const auto primary = properties_->ResolveColor(WidgetPropertyType::COLOR_PRIMARY_ACTIVE, theme.GetPrimaryColor());

    lv_obj_set_style_text_font(lv_label_, theme.GetPrimaryFontLarge().ToLvFont(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(lv_label_, primary.ToLvColor(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(lv_label_, primary.ToLvOpa(), LV_PART_MAIN | LV_STATE_DEFAULT);

    return 0;
}

lv_obj_t* DigitalIndicator::Create(std::shared_ptr<Frame> parent) {
    lv_label_ = lv_label_create(parent->GetObject());

    lv_obj_set_width(lv_label_, LV_SIZE_CONTENT);
    lv_obj_set_height(lv_label_, LV_SIZE_CONTENT);
    lv_label_set_text(lv_label_, "0");

    // Alignment
    lv_obj_center(lv_label_);
    lv_obj_set_align(lv_label_, LV_ALIGN_CENTER);
    lv_obj_set_style_text_align(lv_label_, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN | LV_STATE_DEFAULT);

    return lv_label_;
}

void DigitalIndicator::UpdateIndicator(float value) {
    char value_str[8];
    snprintf(value_str, sizeof(value_str), "%.*f", value_precision_, value);

    lv_label_set_text(lv_label_, value_str);
}

void DigitalIndicator::OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) {
    if(type == WidgetPropertyType::VALUE_PRECISION) {
        value_precision_ = ConfigValueAs<int>(value, 0);
        return;
    }

    IndicatorBase::OnPropertyChanged(type, value);
}

} // namespace eerie_leap::views::widgets::indicators
