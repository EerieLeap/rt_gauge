#pragma once

#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>

#include <lvgl.h>

#include "views/widgets/basic/icons/icon_base.h"

namespace eerie_leap::views::widgets::basic::icons {

// Draws the SVG image asset named by FILE_PATH, rasterized into straight alpha ARGB8888 pixels when it renders, so
// a redraw costs what an image's does. WIDTH_PX x HEIGHT_PX is the icon's size: unset, the document's own; with one
// of them set, the other follows the document's aspect ratio; with both, the document is fitted and centered. All
// three take effect at the next render.
class SvgIcon : public IconBase {
private:
    std::pmr::string file_path_;
    int width_px_ = 0;
    int height_px_ = 0;

    lv_image_dsc_t image_{};
    std::pmr::vector<uint8_t> pixels_;
    // The object that reads image_ and pixels_; null until the first render that drew.
    lv_obj_t* image_object_ = nullptr;

    // Draws the document into pixels_ and describes them in image_.
    bool Rasterize();

public:
    explicit SvgIcon(std::shared_ptr<Frame> parent);
    ~SvgIcon() override;
    SvgIcon(const SvgIcon&) = delete;
    SvgIcon& operator=(const SvgIcon&) = delete;
    SvgIcon(SvgIcon&&) = delete;
    SvgIcon& operator=(SvgIcon&&) = delete;

    static void RegisterProperties(WidgetPropertyStore& store);

    int ApplyTheme(const ITheme& theme) override;
    int DoRender() override;
    void Configure(std::shared_ptr<WidgetPropertyStore> properties) override;

    [[nodiscard]] IconType GetIconType() const override {
        return IconType::Svg;
    }
};

} // namespace eerie_leap::views::widgets::basic::icons
