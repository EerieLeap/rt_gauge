#pragma once

#include <memory_resource>
#include <vector>

#include "domain/ui_domain/models/widget_direction.h"
#include "domain/ui_domain/models/widget_fill_mode.h"
#include "views/widgets/basic/icons/icon_base.h"

namespace eerie_leap::views::widgets::basic::icons {

using eerie_leap::domain::ui_domain::models::WidgetDirection;
using eerie_leap::domain::ui_domain::models::WidgetFillMode;

class ShapeIcon : public IconBase {
private:
    struct Settings {
        int width_px = 32;
        int height_px = 32;
        int stroke_px = 2;
        int corner_radius_px = 0;
        WidgetFillMode fill_mode = WidgetFillMode::Filled;
        WidgetDirection direction = WidgetDirection::LeftToRight;

        bool operator==(const Settings&) const = default;
    };

    IconType type_;
    Settings settings_;
    lv_obj_t* image_object_ = nullptr;
    lv_image_dsc_t image_ {};
    std::pmr::vector<uint8_t> mask_;

    void UpdateImage();
    void Rasterize(lv_vector_path_t* path, int width_px, int height_px, float stroke_px);

public:
    ShapeIcon(std::shared_ptr<Frame> parent, IconType type);
    ~ShapeIcon() override;

    void Configure(std::shared_ptr<WidgetPropertyStore> properties) override;
    int DoRender() override;
    int ApplyTheme(const ITheme& theme) override;
    [[nodiscard]] IconType GetIconType() const override { return type_; }
};

} // namespace eerie_leap::views::widgets::basic::icons
