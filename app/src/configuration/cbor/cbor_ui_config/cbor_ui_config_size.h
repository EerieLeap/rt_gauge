#pragma once

#include "utilities/cbor/cbor_size_builder.hpp"

#include "cbor_ui_config.h"

using eerie_leap::utilities::cbor::CborSizeBuilder;

static size_t GetCborPropertyValueTypeSize(const CborPropertyValueType_r& value) {
    CborSizeBuilder builder;

    switch (value.CborPropertyValueType_choice) {
        case CborPropertyValueType_r::CborPropertyValueType_int_c:
            builder.AddInt(std::get<int32_t>(value.value));
            break;
        case CborPropertyValueType_r::CborPropertyValueType_float_c:
            builder.AddDouble(std::get<double>(value.value));
            break;
        case CborPropertyValueType_r::CborPropertyValueType_tstr_c:
            builder.AddTstr(std::get<zcbor_string>(value.value));
            break;
        case CborPropertyValueType_r::CborPropertyValueType_bool_c:
            builder.AddBool(std::get<bool>(value.value));
            break;
        case CborPropertyValueType_r::CborPropertyValueType_int_l_c: {
            const auto& vec = std::get<std::pmr::vector<int32_t>>(value.value);
            builder.AddIndefiniteArrayStart();
            for(const auto& item : vec)
                builder.AddInt(item);

            break;
        }
        case CborPropertyValueType_r::CborPropertyValueType_tstr_l_c: {
            const auto& vec = std::get<std::pmr::vector<zcbor_string>>(value.value);
            builder.AddIndefiniteArrayStart();
            for(const auto& item : vec)
                builder.AddTstr(item);

            break;
        }
        case CborPropertyValueType_r::CborPropertyValueType_map_c: {
            const auto& vec = std::get<std::pmr::vector<map_tstrtstr>>(value.value);
            builder.AddIndefiniteArrayStart();
            for(const auto& item : vec) {
                builder.AddTstr(item.tstrtstr_key)
                    .AddTstr(item.tstrtstr);
            }
            break;
        }
        default:
            break;
    }

    return builder.Build();
}

static size_t cbor_get_size_CborUiConfig(const CborUiConfig& config) {
    CborSizeBuilder builder;
    builder.AddIndefiniteArrayStart();

    builder.AddUint(config.version)
        .AddUint(config.active_screen_group_id);

    builder.AddOptional(config.properties_present,
        config.properties,
        [](const CborPropertiesConfig& properties) {

        CborSizeBuilder builder;
        builder.AddIndefiniteArrayStart();

        for(const auto& property : properties.CborPropertyValueType_m) {
            builder.AddUint(property.CborPropertyValueType_m_key)
                .AddSize(GetCborPropertyValueTypeSize(property.CborPropertyValueType_m));
        }

        return builder.Build();
    });

    builder.AddIndefiniteArrayStart();
    for(const auto& screen : config.CborScreenConfig_m) {
        builder.AddIndefiniteArrayStart();

        builder.AddUint(screen.id)
            .AddUint(screen.group_id)
            .AddUint(screen.type)
            .AddInt(screen.z_index)
            .AddBool(screen.is_visible)
            .AddBool(screen.is_overlay);

        // CborGridSettingsConfig
        builder.AddIndefiniteArrayStart();
        builder.AddBool(screen.grid.snap_enabled)
            .AddUint(screen.grid.width)
            .AddUint(screen.grid.height)
            .AddUint(screen.grid.spacing_px);

        builder.AddIndefiniteArrayStart();
        for(const auto& widget : screen.CborWidgetConfig_m) {
            builder.AddIndefiniteArrayStart();

            builder.AddUint(widget.type)
                .AddUint(widget.id);

            // CborWidgetPositionConfig
            builder.AddIndefiniteArrayStart();
            builder.AddInt(widget.position.x)
                .AddInt(widget.position.y);

            // CborWidgetSizeConfig
            builder.AddIndefiniteArrayStart();
            builder.AddUint(widget.size.width)
                .AddUint(widget.size.height);

            builder.AddInt(widget.z_index);

            builder.AddOptional(widget.properties_present,
                widget.properties,
                [](const CborPropertiesConfig& properties) {

                CborSizeBuilder builder;
                builder.AddIndefiniteArrayStart();

                for(const auto& property : properties.CborPropertyValueType_m) {
                    builder.AddUint(property.CborPropertyValueType_m_key)
                        .AddSize(GetCborPropertyValueTypeSize(property.CborPropertyValueType_m));
                }

                return builder.Build();
            });

            builder.AddIndefiniteArrayStart();
            for(const auto& binding : widget.CborPropertyBinding_m) {
                builder.AddIndefiniteArrayStart();

                builder.AddUint(binding.target)
                    .AddUint(binding.channel)
                    .AddUint(binding.event_type)
                    .AddUint(binding.payload_key)
                    .AddUint(binding.direction)
                    .AddUint(binding.outbound_event_type)
                    .AddBool(binding.has_selector)
                    .AddUint(binding.selector_key)
                    .AddSize(GetCborPropertyValueTypeSize(binding.selector_value));
            }
        }
    }

    return builder.Build();
}
