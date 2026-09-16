#pragma once

#include <cstdint>

namespace eerie_leap::domain::ui_domain::models {

// Append new properties before COUNT;
// their persisted IDs must never be reordered or reused.
enum class UiPropertyType : std::uint16_t {
    NONE = 0,
    COUNT                  // Sentinel, not a property
};

constexpr bool IsValidUiPropertyType(UiPropertyType type) {
    return type > UiPropertyType::NONE && type < UiPropertyType::COUNT;
}

} // namespace eerie_leap::domain::ui_domain::models
