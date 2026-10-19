#pragma once

#include <cstdint>

namespace eerie_leap::domain::ui_domain::models {

// Append new properties before COUNT;
// their persisted IDs must never be reordered or reused.
enum class UiPropertyType : std::uint16_t {
    NONE = 0,
    COUNT // Sentinel, not a property
};

constexpr bool IsValidUiPropertyType(UiPropertyType type) {
    // NOLINTNEXTLINE(clang-diagnostic-tautological-overlap-compare): no property exists yet, so the valid range is empty until one is added before COUNT
    return type > UiPropertyType::NONE && type < UiPropertyType::COUNT;
}

} // namespace eerie_leap::domain::ui_domain::models
