#include <cstdint>
#include <utility>

#include "domain/ui_domain/models/widget_property.h"

#include "setting_widget_base.h"

namespace eerie_leap::views::widgets {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;

SettingWidgetBase::SettingWidgetBase(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context) :
    WidgetBase(id, std::move(parent), std::move(context)) {}

void SettingWidgetBase::RegisterProperties(WidgetPropertyStore& store) const {
    WidgetBase::RegisterProperties(store);

    store.Register(WidgetPropertyType::SETTING_ID, ConfigValue{ std::pmr::string{} }, PropertyChangeEffect::None);

    // No effect: every subclass renders the value through an LVGL setter that invalidates itself.
    store.Register(WidgetPropertyType::VALUE, ConfigValue{ 0.0 }, PropertyChangeEffect::None);
    store.Register(WidgetPropertyType::MIN_VALUE, ConfigValue{ 0.0 }, PropertyChangeEffect::Relayout);
    store.Register(WidgetPropertyType::MAX_VALUE, ConfigValue{ 0.0 }, PropertyChangeEffect::Relayout);
    store.Register(WidgetPropertyType::STEP, ConfigValue{ 0.0 }, PropertyChangeEffect::Relayout);
}

void SettingWidgetBase::OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) {
    if(type == WidgetPropertyType::SETTING_ID) {
        setting_id_ = ConfigValueAs<std::pmr::string>(value, "");

        return;
    }

    WidgetBase::OnPropertyChanged(type, value);
}

bool SettingWidgetBase::HasSetting() const {
    return !setting_id_.empty();
}

} // namespace eerie_leap::views::widgets
