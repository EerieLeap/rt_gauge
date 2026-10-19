#pragma once

#include <cstdint>

namespace eerie_leap::domain::ui_domain::event_bus {

enum class NavigationEventType : std::uint32_t {
    Changed
};

// The channel bounds HasEventType() with it; appending a value means moving this to it.
inline constexpr NavigationEventType LAST_NAVIGATION_EVENT_TYPE = NavigationEventType::Changed;

} // namespace eerie_leap::domain::ui_domain::event_bus
