#pragma once

#include <cstdint>
#include <memory>

#include <lvgl.h>

#include "views/widgets/controls/control_base.h"

namespace eerie_leap::views::widgets::controls {

class ToggleControl : public ControlBase {
private:
    lv_obj_t* lv_switch_ = nullptr;
    bool shown_checked_ = false;

    void UpdateSwitch();

    int DoRender() override;
    int ApplyTheme(const ITheme& theme) override;

protected:
    void OnControlEvent(lv_event_code_t code) override;
    void RestoreShownValue() override;
    void RegisterProperties(WidgetPropertyStore& store) const override;
    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override;

public:
    explicit ToggleControl(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context);

    WidgetType GetType() const override { return WidgetType::ControlToggle; }
};

} // namespace eerie_leap::views::widgets::controls
