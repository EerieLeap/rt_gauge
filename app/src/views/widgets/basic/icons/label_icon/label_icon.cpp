#include "domain/ui_domain/models/widget_property.h"
#include "views/themes/theme_manager.h"

#include "label_icon.h"

namespace eerie_leap::views::widgets::basic::icons {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::themes;

LabelIcon::LabelIcon(std::shared_ptr<Frame> parent) : IconBase(std::move(parent)) { }

void LabelIcon::RegisterProperties(WidgetPropertyStore& store) {
    IconBase::RegisterProperties(store);

    store.RegisterColor(WidgetPropertyType::COLOR_PRIMARY_ACTIVE);
    store.RegisterColor(WidgetPropertyType::COLOR_SECONDARY_ACTIVE);

    store.Register(WidgetPropertyType::LABEL, ConfigValue { std::pmr::string { } }, PropertyChangeEffect::Rebuild);
}

int LabelIcon::ApplyTheme(const ITheme& theme) {
    const auto primary = properties_->ResolveColor(WidgetPropertyType::COLOR_PRIMARY_ACTIVE, theme.GetAccentColor());
    const auto secondary = properties_->ResolveColor(WidgetPropertyType::COLOR_SECONDARY_ACTIVE, theme.GetPrimaryColor());

    lv_obj_set_style_bg_color(container_->GetObject(), primary.ToLvColor(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(container_->GetObject(), primary.ToLvOpa(), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_text_color(lv_label_, secondary.ToLvColor(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_opa(lv_label_, secondary.ToLvOpa(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_text_font(lv_label_, theme.GetPrimaryFont().ToLvFont(), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_update_layout(lv_label_);

    lv_coord_t label_width = lv_obj_get_width(lv_label_);
    lv_coord_t label_height = lv_obj_get_height(lv_label_);

    if(label_width == 0 || label_height == 0)
        throw std::runtime_error("IconWidget: Label dimensions are invalid.");

    int32_t icon_width = label_width + label_height * 0.75;
    int32_t icon_height = label_height + label_height / 4;

    lv_obj_set_width(container_->GetObject(), icon_width);
    lv_obj_set_height(container_->GetObject(), icon_height);
    lv_obj_set_style_radius(container_->GetObject(), icon_height / 2, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_set_style_transform_pivot_x(container_->GetObject(), icon_width / 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_transform_pivot_y(container_->GetObject(), icon_height / 2, LV_PART_MAIN | LV_STATE_DEFAULT);

    return 0;
}

int LabelIcon::DoRender() {
    auto lv_icon = Create(parent_->GetObject());
    container_ = std::make_shared<Frame>(Frame::Create(lv_icon).Build());

    return 0;
}

lv_obj_t* LabelIcon::Create(lv_obj_t* parent) {
    auto lv_icon = lv_obj_create(parent);
    lv_obj_set_align(lv_icon, LV_ALIGN_CENTER);
    lv_obj_remove_flag(lv_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(lv_icon, 0, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_shadow_opa(lv_icon, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_label_ = lv_label_create(lv_icon);
    lv_obj_set_width(lv_label_, LV_SIZE_CONTENT);
    lv_obj_set_height(lv_label_, LV_SIZE_CONTENT);
    lv_obj_set_align(lv_label_, LV_ALIGN_CENTER);
    lv_label_set_text(lv_label_, label_.c_str());

    return lv_icon;
}

void LabelIcon::Configure(std::shared_ptr<WidgetPropertyStore> properties) {
    IconBase::Configure(std::move(properties));

    label_ = properties_->GetAs<std::pmr::string>(WidgetPropertyType::LABEL, "");
}

} // namespace eerie_leap::views::widgets::basic::icons
