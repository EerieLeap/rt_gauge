#pragma once

#include <string>

#include <lvgl.h>

#include "views/widgets/basic/icons/icon_base.h"

namespace eerie_leap::views::widgets::basic::icons {

class DotIcon : public IconBase {
private:
    lv_obj_t* Create(lv_obj_t* parent);

public:
    explicit DotIcon(std::shared_ptr<Frame> parent);

    int ApplyTheme(const ITheme& theme) override;
    int DoRender() override;
    [[nodiscard]] IconType GetIconType() const override { return IconType::Dot; }
};

} // namespace eerie_leap::views::widgets::basic::icons
