#pragma once

#include <memory>

#include <lvgl.h>

#include "views/widgets/basic/icons/i_icon.h"
#include "views/widgets/widget_base.h"

namespace eerie_leap::views::widgets::basic {

using eerie_leap::domain::ui_domain::models::IconType;
using eerie_leap::views::widgets::basic::icons::IIcon;

class IconWidget : public WidgetBase {
protected:
    IconType icon_type_;
    int position_x_;
    int position_y_;

    std::unique_ptr<IIcon> icon_;

    virtual lv_obj_t* Create();
    int DoRender() override;

public:
    IconWidget(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context, IconType icon_type = IconType::None);
    ~IconWidget() override;
    std::shared_ptr<Frame> GetIconContainer() const;

protected:
    lv_obj_t* GetAnchorObject() const override;
    void ApplyResolvedAnchor(const lv_point_t& point) override;
    void OnProcessingUpdated(bool enabled) override;
    void RegisterProperties(WidgetPropertyStore& store) const override;
    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override;

public:
    int ApplyTheme(const ITheme& theme) override;

    WidgetType GetType() const override { return WidgetType::BasicIcon; }
};

} // namespace eerie_leap::views::widgets::basic
