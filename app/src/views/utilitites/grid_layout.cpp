#include <cstdlib>
#include <stdexcept>

#include <lvgl.h>

#include "grid_layout.h"

namespace eerie_leap::views::utilitites {

GridLayout::GridLayout(const GridSettings& grid, int32_t screen_width, int32_t screen_height) :
    grid_(grid),
    screen_width_(screen_width),
    screen_height_(screen_height) {

    if(screen_width_ <= 0 || screen_height_ <= 0)
        throw std::invalid_argument("Invalid active screen size.");

    if(grid_.width == 0 || grid_.width > static_cast<uint32_t>(screen_width_))
        throw std::invalid_argument("Invalid screen width.");

    if(grid_.height == 0 || grid_.height > static_cast<uint32_t>(screen_height_))
        throw std::invalid_argument("Invalid screen height.");
}

GridLayout GridLayout::FromActiveScreen(const GridSettings& grid) {
    lv_obj_t* active_screen = lv_screen_active();

    return { grid, lv_obj_get_width(active_screen), lv_obj_get_height(active_screen) };
}

WidgetSize GridLayout::ToPx(const WidgetSize& size_grid) const {
    if(size_grid.width == 0)
        throw std::invalid_argument("Invalid widget width.");

    if(size_grid.height == 0)
        throw std::invalid_argument("Invalid widget height.");

    uint32_t width = 0;
    if(grid_.width == size_grid.width)
        width = screen_width_;
    else
        width = (screen_width_ / grid_.width) * size_grid.width - grid_.spacing_px * 2;

    uint32_t height = 0;
    if(grid_.height == size_grid.height)
        height = screen_height_;
    else
        height = (screen_height_ / grid_.height) * size_grid.height - grid_.spacing_px * 2;

    return { .width = width, .height = height };
}

WidgetPosition GridLayout::ToPx(const WidgetPosition& position_grid, const WidgetSize& size_px) const {
    if(size_px.width == 0 || size_px.height == 0)
        throw std::runtime_error("Widget size is not set.");

    uint32_t cell_width = (screen_width_ / grid_.width) + (grid_.spacing_px * 2);
    uint32_t cell_height = (screen_height_ / grid_.height) + (grid_.spacing_px * 2);

    int x = cell_width * position_grid.x;
    int y = screen_height_ - cell_height * position_grid.y - size_px.height;

    return { .x = x, .y = std::abs(y) };
}

} // namespace eerie_leap::views::utilitites
