#include <algorithm>
#include <cmath>

#include "icon_base.h"

#include "domain/ui_domain/lvgl_lock.h"
#include "domain/ui_domain/models/widget_property.h"

namespace eerie_leap::views::widgets::basic::icons {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::utilitites;
using eerie_leap::domain::ui_domain::ScopedLvglLock;

IconBase::IconBase(std::shared_ptr<Frame> parent) :
    parent_(std::move(parent)) {

    container_ = std::make_shared<Frame>(
        Frame::CreateWrapped(parent_->GetObject()).SetWidth(100, false).SetHeight(100, false).Build()
    );
}

void IconBase::SetContext(WidgetContext context) {
    context_ = std::move(context);
}

void IconBase::SetProcessingEnabled(bool enabled) {
    ScopedLvglLock lvgl_guard;
    is_processing_enabled_ = enabled;
}

bool IconBase::IsProcessingEligible() const {
    ScopedLvglLock lvgl_guard;
    if(!IsReady() || !is_processing_enabled_ || !parent_->IsProcessingEnabled())
        return false;

    // The icon's own part opacity is visual; owner management and ancestors still gate processing.
    return Frame::IsVisibleInHierarchy(parent_->GetObject());
}

int IconBase::ReadPixels(const WidgetPropertyStore& store, WidgetPropertyType type, int fallback, bool is_dimension) {
    const double value = store.GetAs<double>(type, fallback);
    if(!std::isfinite(value) || (is_dimension && (value < 0 || value > MAX_DIMENSION)))
        return is_dimension ? -1 : 0;

    return static_cast<int>(std::clamp(value, 0.0, static_cast<double>(MAX_DIMENSION)));
}

void IconBase::Configure(std::shared_ptr<WidgetPropertyStore> properties) {
    properties_ = std::move(properties);
    for(const auto type : properties_->GetRegisteredTypes())
        properties_->ApplyColor(type);
}

} // namespace eerie_leap::views::widgets::basic::icons
