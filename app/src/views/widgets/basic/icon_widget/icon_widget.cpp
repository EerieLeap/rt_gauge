#include "domain/ui_domain/models/widget_property.h"

#include "views/utilitites/positioning_helpers.h"
#include "views/themes/theme_manager.h"

#include "views/widgets/basic/icons/icon_factory.h"

#include "icon_widget.h"

namespace eerie_leap::views::widgets::basic {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::utilitites;
using namespace eerie_leap::views::themes;
using namespace eerie_leap::views::widgets::basic::icons;

IconWidget::IconWidget(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context, IconType icon_type)
    : WidgetBase(id, std::move(parent), std::move(context)), icon_type_(icon_type) {}

IconWidget::~IconWidget() {
    DetachDispatch();
    content_frame_->SetChild(nullptr);
}

std::shared_ptr<Frame> IconWidget::GetIconContainer() const {
    return icon_ != nullptr && icon_->IsReady() ? icon_->GetContainer() : nullptr;
}

lv_obj_t* IconWidget::GetAnchorObject() const {
    const auto frame = GetIconContainer();
    return frame != nullptr ? frame->GetObject() : nullptr;
}

void IconWidget::ApplyResolvedAnchor(const lv_point_t& point) {
    icon_->SetAnchorPoint(point);
}

int IconWidget::DoRender() {
    auto lv_obj = Create();
    if(lv_obj == nullptr)
        return -1;

    content_frame_->SetChild(icon_->GetContainer());

    return 0;
}

int IconWidget::ApplyTheme(const ITheme& theme) {
    icon_->ApplyTheme(theme);

    return 0;
}

lv_obj_t* IconWidget::Create() {
    if(icon_type_ == IconType::None)
        throw std::runtime_error("Invalid icon type.");

    icon_ = IconFactory::GetInstance().Create(icon_type_, properties_, content_frame_);
    icon_->SetAssetsManager(context_.assets_manager);
    if(icon_->Render() != 0)
        return nullptr;

    lv_obj_set_x(icon_->GetContainer()->GetObject(), position_x_);
    lv_obj_set_y(icon_->GetContainer()->GetObject(), position_y_);

    return icon_->GetContainer()->GetObject();
}

void IconWidget::OnProcessingUpdated(bool enabled) {
    if(icon_ != nullptr)
        icon_->SetProcessingEnabled(enabled);
}

void IconWidget::RegisterProperties(WidgetPropertyStore& store) const {
    WidgetBase::RegisterProperties(store);

    store.Register(WidgetPropertyType::ICON_TYPE, ConfigValue { 0 }, PropertyChangeEffect::Rebuild);
    store.Register(WidgetPropertyType::POSITION_X, ConfigValue { 0 }, PropertyChangeEffect::Relayout);
    store.Register(WidgetPropertyType::POSITION_Y, ConfigValue { 0 }, PropertyChangeEffect::Relayout);

    // ICON_TYPE is seeded after registration, so resolve it from configuration
    // here. A constructor-fixed icon (for example a dial's image needle) wins.
    auto type = icon_type_;
    if(type == IconType::None && configuration_ != nullptr) {
        auto it = configuration_->properties.find(WidgetPropertyType::ICON_TYPE);
        if(it != configuration_->properties.end())
            type = static_cast<IconType>(ConfigValueAs<int>(it->second, 0));
    }
    IconFactory::GetInstance().RegisterProperties(type, store);
}

void IconWidget::OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) {
    switch(type) {
        // A concrete type given at construction wins: the needle of a dial is not configurable.
        case WidgetPropertyType::ICON_TYPE:
            if(icon_type_ == IconType::None)
                icon_type_ = static_cast<IconType>(ConfigValueAs<int>(value, 0));
            break;

        case WidgetPropertyType::POSITION_X:
            position_x_ = ConfigValueAs<int>(value, 0);
            if(icon_ != nullptr && icon_->IsReady()) {
                lv_obj_set_x(icon_->GetContainer()->GetObject(), position_x_);
                ApplyTheme(ThemeManager::GetInstance().GetCurrentTheme());
            }
            break;

        case WidgetPropertyType::POSITION_Y:
            position_y_ = ConfigValueAs<int>(value, 0);
            if(icon_ != nullptr && icon_->IsReady()) {
                lv_obj_set_y(icon_->GetContainer()->GetObject(), position_y_);
                ApplyTheme(ThemeManager::GetInstance().GetCurrentTheme());
            }
            break;

        case WidgetPropertyType::WIDTH_PX:
        case WidgetPropertyType::HEIGHT_PX:
        case WidgetPropertyType::IMG_WIDTH:
        case WidgetPropertyType::IMG_HEIGHT:
        case WidgetPropertyType::STROKE_PX:
        case WidgetPropertyType::CORNER_RAD_PX:
        case WidgetPropertyType::FILL_MODE:
        case WidgetPropertyType::DIRECTION:
            if(icon_ != nullptr && icon_->IsReady()) {
                icon_->Configure(properties_);
                ApplyTheme(ThemeManager::GetInstance().GetCurrentTheme());
            }
            break;

        default:
            WidgetBase::OnPropertyChanged(type, value);
            break;
    }
}

} // namespace eerie_leap::views::widgets::basic
