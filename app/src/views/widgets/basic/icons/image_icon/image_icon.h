#pragma once

#include <string>
#include <optional>
#include <memory_resource>

#include <lvgl.h>

#include "views/widgets/basic/icons/icon_base.h"

namespace eerie_leap::views::widgets::basic::icons {

class ImageIcon : public IconBase {
private:
    std::pmr::string file_path_;
    int image_width_ = 0;
    int image_height_ = 0;
    int pivot_x_;
    int pivot_y_;

    lv_image_dsc_t lv_image_descriptor_;
    std::optional<std::pmr::vector<uint8_t>> image_data_;

    lv_obj_t* Create(lv_obj_t* parent);

public:
    // Sentinel for PIVOT_X: a registered property always has a value, so "centre it" needs one.
    static constexpr int pivot_centered = -1;

    explicit ImageIcon(std::shared_ptr<Frame> parent);
    virtual ~ImageIcon() = default;

    static void RegisterProperties(WidgetPropertyStore& store);

    int ApplyTheme(const ITheme& theme) override;
    int DoRender() override;
    void Configure(std::shared_ptr<WidgetPropertyStore> properties) override;

    [[nodiscard]] IconType GetIconType() const override { return IconType::Image; }
};

} // namespace eerie_leap::views::widgets::basic::icons
