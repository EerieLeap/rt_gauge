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

bool IconBase::IsProcessingEligible() const {
    ScopedLvglLock lvgl_guard;
    if(!IsReady() || !is_processing_enabled_ || !parent_->IsProcessingEnabled())
        return false;

    // Start at the owning widget, excluding the icon's own pulse/part opacity.
    for(auto* object = parent_->GetObject(); object != nullptr; object = lv_obj_get_parent(object)) {
        if(lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)
            || lv_obj_get_style_opa(object, LV_PART_MAIN) == LV_OPA_TRANSP
            || lv_obj_get_style_opa_layered(object, LV_PART_MAIN) == LV_OPA_TRANSP
        ) {
            return false;
        }
    }
    return true;
}

void IconBase::Configure(std::shared_ptr<WidgetPropertyStore> properties) {
    properties_ = std::move(properties);
    for(auto type : properties_->GetRegisteredTypes())
        properties_->ApplyColor(type);
}

} // namespace eerie_leap::views::widgets::basic::icons
