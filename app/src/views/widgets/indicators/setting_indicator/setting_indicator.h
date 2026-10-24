#pragma once

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>

#include <lvgl.h>

#include "views/utilities/lvgl_font.h"
#include "views/widgets/setting_widget_base.h"

namespace eerie_leap::views::widgets::indicators {

using eerie_leap::views::widgets::SettingWidgetBase;

// Renders a setting read-only. Not an IndicatorBase: that one is built around a
// sensor id, an EMA filter and a value-change animation, none of which a setting has.
class SettingIndicator : public SettingWidgetBase {
private:
    static constexpr int MAX_VALUE_PRECISION = 9;

    lv_obj_t* lv_label_ = nullptr;
    // The configured font; the theme's when empty.
    std::optional<eerie_leap::views::utilities::LvglFont> font_;

    std::pmr::string label_;
    std::pmr::string unit_;
    int value_precision_ = 0;

    void UpdateText();

    int DoRender() override;
    int ApplyTheme(const ITheme& theme) override;

public:
    explicit SettingIndicator(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context);

protected:
    void RegisterProperties(WidgetPropertyStore& store) const override;
    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override;

public:
    WidgetType GetType() const override {
        return WidgetType::IndicatorSetting;
    }
};

} // namespace eerie_leap::views::widgets::indicators
