#include <cmath>
#include <numbers>

#include <zephyr/kernel.h>

#include "positioning_helpers.h"

namespace eerie_leap::views::utilitites {

WidgetPosition PositioningHelpers::GetPointOnCircle(int center_x, int center_y, int radius_px, float angle_deg) {
    // The angle is rounded to float on purpose: the positions are the ones this precision has always given.
    const auto angle_radians = static_cast<double>(
        static_cast<float>(static_cast<double>(angle_deg) * std::numbers::pi / 180.0)
    );
    const int x = static_cast<int>(center_x + (radius_px * std::cos(angle_radians)));
    const int y = static_cast<int>(center_y + (radius_px * std::sin(angle_radians)));

    return { .x = x, .y = y };
}

void PositioningHelpers::PlaceObjectOnCircle(
    lv_obj_t* object,
    int center_x,
    int center_y,
    int radius_px,
    float angle_deg
) {
    const float angle_deg_normalized = 180.0F + angle_deg;
    WidgetPosition pos = GetPointOnCircle(center_x, center_y, radius_px, angle_deg_normalized);

    const int angle = static_cast<int>(angle_deg_normalized * 10) - 900;

    // NOTE: lv_obj_set_style_transform_rotation causes native_sim
    // to segfault if delay is not added
#ifdef CONFIG_BOARD_NATIVE_SIM
    k_sleep(K_TICKS(1));
#endif // CONFIG_BOARD_NATIVE_SIM
    lv_obj_set_style_transform_rotation(object, angle, 0);
    lv_obj_set_x(object, pos.x);
    lv_obj_set_y(object, pos.y);
}

} // namespace eerie_leap::views::utilitites
