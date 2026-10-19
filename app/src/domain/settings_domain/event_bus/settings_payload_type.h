#pragma once

#include <cstdint>

namespace eerie_leap::domain::settings_domain::event_bus {

// Persisted as the payload key of a widget binding - append only.
enum class SettingsPayloadType : std::uint32_t {
    SettingId,
    Value,
    MinValue,
    MaxValue,
    Step
};

// The channel bounds HasPayloadKey() with it; appending a value means moving this to it.
inline constexpr SettingsPayloadType LAST_SETTINGS_PAYLOAD_TYPE = SettingsPayloadType::Step;

} // namespace eerie_leap::domain::settings_domain::event_bus
