#include "domain/ui_domain/models/widget_property.h"

#include "views/themes/theme_manager.h"
#include "views/utilitites/positioning_helpers.h"

#include "arc_icon_widget.h"

namespace eerie_leap::views::widgets::basic {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::utilitites;
using namespace eerie_leap::views::themes;

ArcIconWidget::ArcIconWidget(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context, IconType icon_type) :
    IconWidget(id, std::move(parent), std::move(context), icon_type) {}

int ArcIconWidget::ApplyTheme(const ITheme& theme) {
    IconWidget::ApplyTheme(theme);

    lv_obj_update_layout(container_->GetObject());

    const int radius = lv_obj_get_height(container_->GetObject()) / 2;
    const lv_coord_t icon_height = lv_obj_get_height(icon_->GetContainer()->GetObject());

    PositioningHelpers::PlaceObjectOnCircle(
        icon_->GetContainer()->GetObject(), position_x_, position_y_, radius - icon_height / 2 - edge_offset_px_,
        position_angle_ - 90.0F
    );

    return 0;
}

void ArcIconWidget::RegisterProperties(WidgetPropertyStore& store) const {
    IconWidget::RegisterProperties(store);

    store.Register(WidgetPropertyType::POSITION_ANGLE, ConfigValue{ -90.0 }, PropertyChangeEffect::Relayout);
    store.Register(WidgetPropertyType::EDGE_OFFSET, ConfigValue{ 0 }, PropertyChangeEffect::Relayout);
}

void ArcIconWidget::OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) {
    switch(type) {
        case WidgetPropertyType::POSITION_ANGLE:
            position_angle_ = static_cast<float>(ConfigValueAs<double>(value, -90.0));
            break;

        case WidgetPropertyType::EDGE_OFFSET:
            edge_offset_px_ = ConfigValueAs<int>(value, 0);
            break;

        default:
            IconWidget::OnPropertyChanged(type, value);
            break;
    }
}

} // namespace eerie_leap::views::widgets::basic
