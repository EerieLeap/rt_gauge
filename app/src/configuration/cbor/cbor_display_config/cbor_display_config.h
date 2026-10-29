/*
 * Generated using zcbor version 0.9.1
 * https://github.com/NordicSemiconductor/zcbor
 * Generated with a --default-max-qty of 24
 */

#pragma once

#include <cstdint>
#include <cstdbool>
#include <cstddef>

#include <zcbor_common.h>

struct CborDisplayConfig {
        uint32_t version;
        uint32_t brightness;
        bool blanking_enabled;
        uint32_t screen_timeout_s;
        uint32_t theme_id;
        struct zcbor_string brightness_sensor_id;
};
