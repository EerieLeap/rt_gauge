#pragma once

#include "utilities/cbor/cbor_size_builder.hpp"

#include "cbor_display_config.h"

using eerie_leap::utilities::cbor::CborSizeBuilder;

static size_t cbor_get_size_CborDisplayConfig(const CborDisplayConfig& config) {
    CborSizeBuilder builder;
    builder.AddIndefiniteArrayStart();

    builder.AddUint(config.version)
        .AddUint(config.brightness)
        .AddBool(config.blanking_enabled)
        .AddUint(config.screen_timeout_s)
        .AddUint(config.theme_id)
        .AddTstr(config.brightness_sensor_id);

    return builder.Build();
}
