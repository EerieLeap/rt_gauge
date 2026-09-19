#pragma once

#include <cstdint>
#include <limits>

namespace eerie_leap::domain::ui_domain::models {

class Animation {
public:
    // Persisted values: append new effects without reordering or reusing existing IDs.
    enum class Type : std::uint8_t {
        None = 0,
        Blinking = 1,
        Rotation = 2
    };

    static constexpr Type DEFAULT_TYPE = Type::None;
    static constexpr bool DEFAULT_ACTIVE = false;
    // Duration is milliseconds per complete cycle, including both halves of a blink.
    static constexpr int DEFAULT_DURATION_MS = 1000;
    static constexpr int MIN_DURATION_MS = 2;
    static constexpr int MAX_DURATION_MS = std::numeric_limits<std::int32_t>::max();
};

} // namespace eerie_leap::domain::ui_domain::models
