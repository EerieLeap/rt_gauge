#include "views/themes/theme_manager.h"

#include "dot_icon.h"

namespace eerie_leap::views::widgets::basic::icons {

using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::themes;

DotIcon::DotIcon(std::shared_ptr<Frame> parent) : IconBase(std::move(parent)) { }

void DotIcon::RegisterProperties(WidgetPropertyStore& store) {
    IconBase::RegisterProperties(store);
    store.RegisterColor(WidgetPropertyType::COLOR_PRIMARY_ACTIVE);
}

int DotIcon::ApplyTheme(const ITheme& theme) {
    const auto primary = properties_->ResolveColor(WidgetPropertyType::COLOR_PRIMARY_ACTIVE, theme.GetAccentColor());

    lv_obj_set_style_bg_color(container_->GetObject(), primary.ToLvColor(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(container_->GetObject(), primary.ToLvOpa(), LV_PART_MAIN | LV_STATE_DEFAULT);

    lv_obj_update_layout(container_->GetObject());

    lv_coord_t icon_width = lv_obj_get_width(container_->GetObject());
    lv_coord_t icon_height = lv_obj_get_height(container_->GetObject());

    if(icon_width == 0 || icon_height == 0)
        throw std::runtime_error("Icon dimensions are invalid.");

    lv_obj_set_style_transform_pivot_x(container_->GetObject(), icon_width / 2, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_transform_pivot_y(container_->GetObject(), icon_height / 2, LV_PART_MAIN | LV_STATE_DEFAULT);

    return 0;
}

int DotIcon::DoRender() {
    auto lv_icon = Create(parent_->GetObject());
    container_ = std::make_shared<Frame>(Frame::Create(lv_icon).Build());

    return 0;
}

lv_obj_t* DotIcon::Create(lv_obj_t* parent) {
    auto lv_icon = lv_obj_create(parent);
    lv_obj_set_width(lv_icon, 16);
    lv_obj_set_height(lv_icon, 16);
    lv_obj_set_align(lv_icon, LV_ALIGN_CENTER);
    lv_obj_remove_flag(lv_icon, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(lv_icon, 8, LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(lv_icon, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    return lv_icon;
}

} // namespace eerie_leap::views::widgets::basic::icons
