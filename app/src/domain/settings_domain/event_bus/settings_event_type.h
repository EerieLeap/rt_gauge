#pragma once

#include <cstdint>

namespace eerie_leap::domain::settings_domain::event_bus {

// Persisted as the event of a widget binding - append only.
enum class SettingsEventType : std::uint32_t {
    // Owner -> UI. The value as it settled, after any clamping.
    Changed,

    // Owner -> UI. The bounds a control has to lay itself out against.
    RangeChanged,

    // UI -> owner. A request, which the owner may clamp or refuse; only Changed is fact.
    ChangeRequested,

    // UI -> owner: republish the current value and range. A widget built after the owner
    // started has no other way to catch up on a channel that only publishes on change.
    StateRequested,

    // Debounce or an explicit save -> owner.
    PersistRequested
};

// The channel bounds HasEventType() with it; appending a value means moving this to it.
inline constexpr SettingsEventType LAST_SETTINGS_EVENT_TYPE = SettingsEventType::PersistRequested;

} // namespace eerie_leap::domain::settings_domain::event_bus
