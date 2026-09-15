#pragma once

#include <string>

#include <lvgl.h>

#include "views/widgets/basic/icons/icon_base.h"

namespace eerie_leap::views::widgets::basic::icons {

class LabelIcon : public IconBase {
private:
    std::pmr::string label_;
    lv_obj_t* lv_label_;

    lv_obj_t* Create(lv_obj_t* parent);

public:
    explicit LabelIcon(std::shared_ptr<Frame> parent);

    static void RegisterProperties(WidgetPropertyStore& store);

    int ApplyTheme(const ITheme& theme) override;
    int DoRender() override;
    void Configure(std::shared_ptr<WidgetPropertyStore> properties) override;

    [[nodiscard]] IconType GetIconType() const override { return IconType::Label; }
};

} // namespace eerie_leap::views::widgets::basic::icons
