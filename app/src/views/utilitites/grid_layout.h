#pragma once

#include <cstdint>

#include "domain/ui_domain/models/grid_settings.h"
#include "domain/ui_domain/models/widget_position.h"
#include "domain/ui_domain/models/widget_size.h"

namespace eerie_leap::views::utilitites {

using eerie_leap::domain::ui_domain::models::GridSettings;
using eerie_leap::domain::ui_domain::models::WidgetPosition;
using eerie_leap::domain::ui_domain::models::WidgetSize;

// Converts grid-relative widget geometry into pixels for a given screen size.
class GridLayout {
private:
    GridSettings grid_;
    int32_t screen_width_;
    int32_t screen_height_;

public:
    GridLayout(const GridSettings& grid, int32_t screen_width, int32_t screen_height);

    static GridLayout FromActiveScreen(const GridSettings& grid);

    const GridSettings& GetGrid() const;

    WidgetSize ToPx(const WidgetSize& size_grid) const;
    WidgetPosition ToPx(const WidgetPosition& position_grid, const WidgetSize& size_px) const;
};

} // namespace eerie_leap::views::utilitites
