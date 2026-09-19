#pragma once

#include <cstddef>
#include <limits>
#include <string>
#include <variant>

#include "domain/ui_domain/utilities/widget_property_validator.h"
#include "subsys/event_bus/event.h"

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/type/config_value.h"

namespace eerie_leap::views::widgets {

using eerie_leap::domain::ui_domain::models::WidgetPropertyType;
using eerie_leap::domain::ui_domain::utilities::WidgetPropertyValidator;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::utilities::type::ConfigValue;

// EventData and ConfigValue overlap but are not the same variant: the bus has no monostate and
// carries uint32/float, the configuration model carries double and PMR strings.
//
// An inbound value coerces to the alternative the property was registered with rather than to
// whatever the publisher happened to send, so a widget's members never change type under it.
// Monostate means the value cannot fill that property, and the event is dropped.
inline ConfigValue CoerceToConfigValue(
    const EventData& value, size_t alternative, WidgetPropertyType type = WidgetPropertyType::NONE) {
    if(type == WidgetPropertyType::IS_ACTIVE || type == WidgetPropertyType::IS_VISIBLE
        || type == WidgetPropertyType::IS_ANIMATION_ACTIVE) {
        return std::visit([](const auto& argument) -> ConfigValue {
            using T = std::decay_t<decltype(argument)>;
            if constexpr(std::is_same_v<T, bool>)
                return argument;
            else if constexpr(std::is_arithmetic_v<T>) {
                if(argument == 0 || argument == 1)
                    return argument == 1;
            }
            return std::monostate{};
        }, value);
    }

    if(WidgetPropertyValidator::IsAnimationProperty(type)) {
        ConfigValue converted;
        if(const auto* number = std::get_if<int>(&value))
            converted = *number;
        else if(const auto* number = std::get_if<uint32_t>(&value)) {
            using namespace eerie_leap::domain::ui_domain::models;
            const uint32_t minimum = type == WidgetPropertyType::ANIMATION_TYPE ? 0 : Animation::MIN_DURATION_MS;
            const uint32_t maximum = type == WidgetPropertyType::ANIMATION_TYPE
                ? static_cast<uint32_t>(Animation::Type::Rotation) : Animation::MAX_DURATION_MS;
            if(*number >= minimum && *number <= maximum
                && *number <= static_cast<uint32_t>(std::numeric_limits<int>::max()))
                converted = static_cast<int>(*number);
        }

        return WidgetPropertyValidator::IsValidAnimationValue(type, converted) ? converted : ConfigValue{};
    }

    if(type == WidgetPropertyType::OPACITY) {
        if(const auto* opacity = std::get_if<int>(&value); opacity != nullptr && *opacity >= 0 && *opacity <= 255)
            return *opacity;
        if(const auto* opacity = std::get_if<uint32_t>(&value); opacity != nullptr && *opacity <= 255)
            return static_cast<int>(*opacity);
        return std::monostate{};
    }

    if(WidgetPropertyValidator::IsColorProperty(type)) {
        if(const auto* text = std::get_if<std::string>(&value))
            return std::pmr::string(*text, Mrm::GetExtPmr());
        uint32_t color_int;
        if(const auto* color = std::get_if<uint32_t>(&value))
            color_int = *color;
        else if(const auto* color = std::get_if<int>(&value))
            color_int = static_cast<uint32_t>(*color);
        else if(const auto* color = std::get_if<float>(&value))
            color_int = static_cast<uint32_t>(*color);
        else
            return std::monostate{};

        constexpr char hex_digits[] = "0123456789ABCDEF";
        char color_str[] = "#000000FF";
        for(size_t i = 6; i > 0; --i) {
            color_str[i] = hex_digits[color_int & 0xF];
            color_int >>= 4;
        }

        return ConfigValue(std::in_place_type<std::pmr::string>, color_str, sizeof(color_str) - 1, Mrm::GetExtPmr());
    }

    return std::visit([alternative](auto&& argument) -> ConfigValue {
        using T = std::decay_t<decltype(argument)>;

        constexpr bool is_number = std::is_same_v<T, int>
            || std::is_same_v<T, uint32_t>
            || std::is_same_v<T, float>;

        switch(alternative) {
            case 1: // int
                if constexpr (is_number)
                    return static_cast<int>(argument);
                else if constexpr (std::is_same_v<T, bool>)
                    return argument ? 1 : 0;
                break;

            case 2: // double
                if constexpr (is_number)
                    return static_cast<double>(argument);
                else if constexpr (std::is_same_v<T, bool>)
                    return argument ? 1.0 : 0.0;
                break;

            case 3: // std::pmr::string
                if constexpr (std::is_same_v<T, std::string>)
                    return std::pmr::string(argument.c_str(), Mrm::GetExtPmr());
                break;

            case 4: // bool
                if constexpr (std::is_same_v<T, bool>)
                    return argument;
                else if constexpr (is_number)
                    return argument != T{ };
                break;

            default:
                break;
        }

        return std::monostate{ };
    }, value);
}

inline EventData ToEventData(const ConfigValue& value) {
    return std::visit([](auto&& argument) -> EventData {
        using T = std::decay_t<decltype(argument)>;

        if constexpr (std::is_same_v<T, int>)
            return argument;
        else if constexpr (std::is_same_v<T, double>)
            return static_cast<float>(argument);
        else if constexpr (std::is_same_v<T, bool>)
            return argument;
        else if constexpr (std::is_same_v<T, std::pmr::string>)
            return std::string(argument.c_str());
        else
            return 0;
    }, value);
}

} // namespace eerie_leap::views::widgets
