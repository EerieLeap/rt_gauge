#include <utility>

#include "domain/ui_domain/models/widget_property.h"

#include "toggle_control.h"

namespace eerie_leap::views::widgets::controls {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::utilitites;
using namespace eerie_leap::views::themes;

ToggleControl::ToggleControl(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context)
    : ControlBase(id, std::move(parent), std::move(context), true) {}

// A toggle carries a boolean where the base assumes a number.
void ToggleControl::RegisterProperties(WidgetPropertyStore& store) const {
    ControlBase::RegisterProperties(store);

    store.RegisterColor(WidgetPropertyType::COLOR_PRIMARY_ACTIVE);
    store.RegisterColor(WidgetPropertyType::COLOR_PRIMARY_INACTIVE);
    store.RegisterColor(WidgetPropertyType::COLOR_SECONDARY_ACTIVE);
    store.RegisterColor(WidgetPropertyType::COLOR_SECONDARY_INACTIVE);

    store.Register(WidgetPropertyType::VALUE, ConfigValue { false }, PropertyChangeEffect::None);
}

int ToggleControl::ApplyTheme(const ITheme& theme) {
    const auto active = properties_->ResolveColor(WidgetPropertyType::COLOR_PRIMARY_ACTIVE, theme.GetPrimaryColor());
    const auto inactive = properties_->ResolveColor(WidgetPropertyType::COLOR_PRIMARY_INACTIVE, theme.GetSurfaceColor());

    lv_obj_set_style_bg_color(lv_switch_, inactive.ToLvColor(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(lv_switch_, inactive.ToLvOpa(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(lv_switch_, active.ToLvColor(), LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(lv_switch_, LV_OPA_TRANSP, LV_PART_INDICATOR | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_color(lv_switch_, active.ToLvColor(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(lv_switch_, active.ToLvOpa(), LV_PART_INDICATOR | LV_STATE_CHECKED);

    for(bool checked : { false, true }) {
        const auto state = checked ? LV_STATE_CHECKED : LV_STATE_DEFAULT;
        const auto secondary = properties_->ResolveColor(checked ? WidgetPropertyType::COLOR_SECONDARY_ACTIVE
            : WidgetPropertyType::COLOR_SECONDARY_INACTIVE, theme.GetAccentColor());
        lv_obj_set_style_bg_color(lv_switch_, secondary.ToLvColor(), LV_PART_KNOB | state);
        lv_obj_set_style_bg_opa(lv_switch_, secondary.ToLvOpa(), LV_PART_KNOB | state);
    }

    return 0;
}

void ToggleControl::OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) {
    ControlBase::OnPropertyChanged(type, value);

    if(type == WidgetPropertyType::VALUE)
        UpdateSwitch();
}

int ToggleControl::DoRender() {
    lv_switch_ = lv_switch_create(content_frame_->GetObject());

    lv_obj_center(lv_switch_);

    AttachEvents(lv_switch_, { LV_EVENT_VALUE_CHANGED });

    UpdateSwitch();

    content_frame_->SetChild(std::make_shared<Frame>(Frame::Create(lv_switch_).Build()));

    return 0;
}

void ToggleControl::OnControlEvent(lv_event_code_t code) {
    if(code != LV_EVENT_VALUE_CHANGED)
        return;

    SetPropertyLocal(
        WidgetPropertyType::VALUE,
        ConfigValue { lv_obj_has_state(lv_switch_, LV_STATE_CHECKED) });}

void ToggleControl::UpdateSwitch() {
    if(lv_switch_ == nullptr)
        return;

    if(properties_->GetAs<bool>(WidgetPropertyType::VALUE, false))
        lv_obj_add_state(lv_switch_, LV_STATE_CHECKED);
    else
        lv_obj_remove_state(lv_switch_, LV_STATE_CHECKED);
}

} // namespace eerie_leap::views::widgets::controls
