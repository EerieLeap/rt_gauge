#pragma once

#include <memory>
#include <memory_resource>

#include "views/widgets/widget_base.h"

namespace eerie_leap::views::widgets {

using eerie_leap::utilities::type::ConfigValue;

// The property surface a setting-bound widget shares: the id it is configured against and the
// four values an owner drives. Which channel fills them is a binding's business, not this class's.
class SettingWidgetBase : public WidgetBase {
protected:
    std::pmr::string setting_id_;

    SettingWidgetBase(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context);

    bool HasSetting() const;

public:
    ~SettingWidgetBase() override = default;

protected:
    void RegisterProperties(WidgetPropertyStore& store) const override;
    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override;
};

} // namespace eerie_leap::views::widgets
