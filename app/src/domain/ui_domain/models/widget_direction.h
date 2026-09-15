#pragma once

#include <cstdint>

namespace eerie_leap::domain::ui_domain::models {

// Persisted through DIRECTION; preserve these numeric values.
enum class WidgetDirection : uint8_t {
    None = 0,
    LeftToRight,
    RightToLeft,
    TopToBottom,
    BottomToTop
};

} // namespace eerie_leap::domain::ui_domain::models
