#pragma once

#include <array>
#include <string_view>

namespace eerie_leap::domain::settings_domain::models {

// Identifies a setting on SettingsEventsChannel - hashed into the SettingId payload - and is
// what a widget's SETTING_ID property refers to. Persisted inside widget configurations, so
// treat them as a wire format: add, never rename.
class SettingId {
public:
    static constexpr const char* DISPLAY_BRIGHTNESS = "display.brightness";

    // Every setting the unit carries, which the device INFO lists.
    static constexpr std::array<std::string_view, 1> ALL{ DISPLAY_BRIGHTNESS };
};

} // namespace eerie_leap::domain::settings_domain::models
