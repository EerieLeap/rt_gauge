#include "domain/ui_domain/models/widget_property.h"
#include "domain/ui_domain/models/widget_direction.h"
#include "domain/ui_domain/models/widget_fill_mode.h"

#include "views/utilitites/positioning_helpers.h"
#include "views/themes/theme_manager.h"

#include "views/widgets/basic/icons/icon_factory.h"
#include "views/widgets/basic/icons/image_icon/image_icon.h"

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
    container_->SetChild(nullptr);
}

int IconWidget::DoRender() {
    auto lv_obj = Create();
    if(lv_obj == nullptr)
        return -1;

    container_->SetChild(icon_->GetContainer());

    return 0;
}

int IconWidget::ApplyTheme(const ITheme& theme) {
    icon_->ApplyTheme(theme);

    return 0;
}

lv_obj_t* IconWidget::Create() {
    if(icon_type_ == IconType::None)
        throw std::runtime_error("Invalid icon type.");

    icon_ = IconFactory::GetInstance().Create(icon_type_, properties_, container_);
    icon_->SetAssetsManager(context_.assets_manager);
    if(icon_->Render() != 0)
        return nullptr;

    lv_obj_set_x(icon_->GetContainer()->GetObject(), position_x_);
    lv_obj_set_y(icon_->GetContainer()->GetObject(), position_y_);

    return icon_->GetContainer()->GetObject();
}

void IconWidget::SetIsActive(bool is_active) {
    if(icon_ != nullptr)
        icon_->SetIsActive(is_active);
}

void IconWidget::RegisterProperties(WidgetPropertyStore& store) const {
    WidgetBase::RegisterProperties(store);

    store.Register(WidgetPropertyType::ICON_TYPE, ConfigValue { 0 }, PropertyChangeEffect::Rebuild);
    store.Register(WidgetPropertyType::POSITION_X, ConfigValue { 0 }, PropertyChangeEffect::Relayout);
    store.Register(WidgetPropertyType::POSITION_Y, ConfigValue { 0 }, PropertyChangeEffect::Relayout);
    store.Register(WidgetPropertyType::IS_ACTIVE, ConfigValue { true }, PropertyChangeEffect::Repaint);

    // Consumed by the IIcon this widget builds in DoRender. Declared here because the icon is
    // chosen by ICON_TYPE, so which of these apply is not known until the replay has run.
    store.Register(WidgetPropertyType::LABEL, ConfigValue { std::pmr::string { } }, PropertyChangeEffect::Rebuild);
    store.Register(WidgetPropertyType::FILE_PATH, ConfigValue { std::pmr::string { } }, PropertyChangeEffect::Rebuild);
    store.Register(WidgetPropertyType::IMG_WIDTH, ConfigValue { 0 }, PropertyChangeEffect::Rebuild);
    store.Register(WidgetPropertyType::IMG_HEIGHT, ConfigValue { 0 }, PropertyChangeEffect::Rebuild);
    store.Register(WidgetPropertyType::PIVOT_X, ConfigValue { ImageIcon::pivot_centered }, PropertyChangeEffect::Rebuild);
    store.Register(WidgetPropertyType::PIVOT_Y, ConfigValue { 0 }, PropertyChangeEffect::Rebuild);
    store.Register(WidgetPropertyType::WIDTH_PX, ConfigValue { 32 }, PropertyChangeEffect::Relayout);
    store.Register(WidgetPropertyType::HEIGHT_PX, ConfigValue { 32 }, PropertyChangeEffect::Relayout);
    store.Register(WidgetPropertyType::STROKE_PX, ConfigValue { 2 }, PropertyChangeEffect::Relayout);
    store.Register(WidgetPropertyType::CORNER_RAD_PX, ConfigValue { 0 }, PropertyChangeEffect::Repaint);
    store.Register(WidgetPropertyType::FILL_MODE,
        ConfigValue { static_cast<int>(WidgetFillMode::Filled) }, PropertyChangeEffect::Repaint);
    store.Register(WidgetPropertyType::DIRECTION,
        ConfigValue { static_cast<int>(WidgetDirection::LeftToRight) }, PropertyChangeEffect::Relayout);
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

        case WidgetPropertyType::IS_ACTIVE:
            SetIsActive(ConfigValueAs<bool>(value, false));
            break;

        case WidgetPropertyType::WIDTH_PX:
        case WidgetPropertyType::HEIGHT_PX:
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
