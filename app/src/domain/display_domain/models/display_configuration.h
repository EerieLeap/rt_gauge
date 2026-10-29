#pragma once

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string>

namespace eerie_leap::domain::display_domain::models {

struct DisplayConfiguration {
    using allocator_type = std::pmr::polymorphic_allocator<>;

    // The brightness the unit stores, starts with and keeps when no sensor drives the panel. A value
    // a sensor sets is never stored here.
    uint8_t brightness = 160;
    bool blanking_enabled = false;

    // 0 = never blank on inactivity.
    uint32_t screen_timeout_s = 0;
    uint32_t theme_id = 0;

    // The sensor the panel's brightness follows; empty for none.
    std::pmr::string brightness_sensor_id;

    DisplayConfiguration() = default;

    DisplayConfiguration(std::allocator_arg_t, allocator_type alloc) :
        brightness_sensor_id(alloc) {}
};

} // namespace eerie_leap::domain::display_domain::models
