#pragma once

#include <cstdint>

namespace eerie_leap::domain::ui_domain::event_bus {

enum class NavigationPayloadType : std::uint32_t {
    Action,
    TargetGroupId,
    TargetScreenId
};

// The channel bounds HasPayloadKey() with it; appending a value means moving this to it.
inline constexpr NavigationPayloadType LAST_NAVIGATION_PAYLOAD_TYPE = NavigationPayloadType::TargetScreenId;

} // namespace eerie_leap::domain::ui_domain::event_bus
