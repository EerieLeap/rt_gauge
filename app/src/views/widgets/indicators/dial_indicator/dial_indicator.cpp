#include <zephyr/kernel.h>

#include "domain/ui_domain/models/widget_property.h"

#include "views/widgets/indicators/indicator_base.h"
#include "views/themes/theme_manager.h"

#include "dial_indicator.h"

namespace eerie_leap::views::widgets::indicators {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::utilitites;

DialIndicator::DialIndicator(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context)
    : IndicatorBase(id, std::move(parent), std::move(context)) {

    // Built here rather than in Create() so it is declarable as a dependency before any render.
    needle_icon_ = std::make_unique<IconWidget>(id_, container_, context_, IconType::Image);

    AddDependency(*needle_icon_);
}

DialIndicator::~DialIndicator() {
    ScopedLvglLock lvgl_guard;
    DetachDispatch();
    OnProcessingSuspended();
    container_->SetChild(nullptr);
    needle_icon_.reset();
    dependencies_.clear();
}

int DialIndicator::DoRender() {
    auto lv_obj = Create();
    if(lv_obj == nullptr)
        return -1;

    // Share the needle's Frame; a second wrapper would also own/delete the same LVGL object.
    container_->SetChild(needle_icon_->GetContainer());

    return 0;
}

int DialIndicator::ApplyTheme(const ITheme& theme) {
    needle_icon_->ApplyTheme(theme);

    return 0;
}

lv_obj_t* DialIndicator::Create() {
    if(needle_icon_->Render() != 0)
        return nullptr;

    lv_needle_icon_ = needle_icon_->GetContainer()->GetChild()->GetObject();
    UpdateIndicator(range_start_);

    return needle_icon_->GetContainer()->GetObject();
}

uint32_t DialIndicator::GetAngleForValue(float value) {
    float resolution = (abs(end_angle_ - start_angle_) * 10) / abs(range_end_ - range_start_);
    float angle_delta = resolution * value;

    return start_angle_ * 10 - 1800 + static_cast<uint32_t>(angle_delta);
}

void DialIndicator::UpdateIndicator(float value) {
    lv_image_set_rotation(
        lv_needle_icon_,
        GetAngleForValue(value));

    // NOTE: lv_image_set_pivot is meant to work along with lv_image_set_rotation,
    // but it does not work as expected out of the box. There is a patch applyed to LVGL
    // in order to fix that bug. Transform is another option here, but it adds artifacts
    // around the image when rotated.
    // lv_obj_set_style_transform_rotation(
    //     lv_needle_icon_,
    //     GetAngleForValue(value),
    //     LV_PART_MAIN | LV_STATE_DEFAULT);
}

void DialIndicator::RegisterProperties(WidgetPropertyStore& store) const {
    IndicatorBase::RegisterProperties(store);

    store.Register(WidgetPropertyType::START_ANGLE, ConfigValue { DEFAULT_START_ANGLE }, PropertyChangeEffect::Repaint);
    store.Register(WidgetPropertyType::END_ANGLE, ConfigValue { DEFAULT_END_ANGLE }, PropertyChangeEffect::Repaint);
}

void DialIndicator::OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) {
    switch(type) {
        case WidgetPropertyType::START_ANGLE:
            start_angle_ = ConfigValueAs<int>(value, DEFAULT_START_ANGLE);
            break;

        case WidgetPropertyType::END_ANGLE:
            end_angle_ = ConfigValueAs<int>(value, DEFAULT_END_ANGLE);
            break;

        default:
            IndicatorBase::OnPropertyChanged(type, value);
            break;
    }
}

} // namespace eerie_leap::views::widgets::indicators
