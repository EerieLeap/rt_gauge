#include "icon_base.h"

#include "domain/ui_domain/models/widget_property.h"
#include "domain/ui_domain/lvgl_lock.h"

namespace eerie_leap::views::widgets::basic::icons {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::utilitites;
using eerie_leap::domain::ui_domain::ScopedLvglLock;

IconBase::IconBase(std::shared_ptr<Frame> parent)
    : parent_(std::move(parent)) {

    container_ = std::make_shared<Frame>(Frame::CreateWrapped(parent_->GetObject())
        .SetWidth(100, false)
        .SetHeight(100, false)
        .Build());
}

void IconBase::SetAssetsManager(std::shared_ptr<AssetsManager> ui_assets_manager) {
    ui_assets_manager_ = std::move(ui_assets_manager);
}

void IconBase::SetProcessingEnabled(bool enabled) {
    ScopedLvglLock lvgl_guard;
    is_processing_enabled_ = enabled;
}

void IconBase::SetAnchorPoint(const lv_point_t& point) {
    auto* object = container_->GetObject();
    if(lv_obj_get_style_transform_pivot_x(object, LV_PART_MAIN) != point.x)
        lv_obj_set_style_transform_pivot_x(object, point.x, LV_PART_MAIN);
    if(lv_obj_get_style_transform_pivot_y(object, LV_PART_MAIN) != point.y)
        lv_obj_set_style_transform_pivot_y(object, point.y, LV_PART_MAIN);
}

bool IconBase::IsProcessingEligible() const {
    ScopedLvglLock lvgl_guard;
    if(!IsReady() || !is_processing_enabled_ || !parent_->IsProcessingEnabled())
        return false;

    // The icon's own part opacity is visual; owner management and ancestors still gate processing.
    return Frame::IsVisibleInHierarchy(parent_->GetObject());
}

void IconBase::Configure(std::shared_ptr<WidgetPropertyStore> properties) {
    properties_ = std::move(properties);
    for(auto type : properties_->GetRegisteredTypes())
        properties_->ApplyColor(type);
}

} // namespace eerie_leap::views::widgets::basic::icons
