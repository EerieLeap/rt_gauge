#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
namespace detail {

inline ConfigValue CoerceAnchorValue(const EventData& value, WidgetPropertyType type) {
    ConfigValue converted;
    if(const auto* coordinate = std::get_if<int>(&value)) {
        converted = *coordinate;
    } else if(const auto* unsigned_coordinate = std::get_if<uint32_t>(&value);
        unsigned_coordinate != nullptr && *unsigned_coordinate <= WidgetPropertyValidator::MAX_ANCHOR_COORDINATE) {
        converted = static_cast<int>(*unsigned_coordinate);
    }
    return WidgetPropertyValidator::IsValidAnchorValue(type, converted) ? converted : ConfigValue{};
}

// A box coordinate or extent in grid cells, a whole number: what a publisher sends rounds to the nearest, and
// a value that is no number (NaN, an infinity), a flag or a text fills nothing. A number past the range a box
// can have is held at its end, so no conversion leaves the range of an int.
inline ConfigValue CoerceLayoutValue(const EventData& value) {
    constexpr float LIMIT = static_cast<float>(1 << 24);

    if(const auto* integer = std::get_if<int>(&value))
        return std::clamp(*integer, -(1 << 24), 1 << 24);

    if(const auto* unsigned_integer = std::get_if<uint32_t>(&value))
        return static_cast<int>(std::min<uint32_t>(*unsigned_integer, 1U << 24));

    if(const auto* fractional = std::get_if<float>(&value)) {
        if(!std::isfinite(*fractional))
            return std::monostate{};

        return static_cast<int>(std::lround(std::clamp(*fractional, -LIMIT, LIMIT)));
    }

    return std::monostate{};
}

// Only 0 and 1 stand for a flag.
inline ConfigValue CoerceFlagValue(const EventData& value) {
    return std::visit(
        [](const auto& argument) -> ConfigValue {
            using T = std::decay_t<decltype(argument)>;
            if constexpr(std::is_same_v<T, bool>) {
                return argument;
            } else if constexpr(std::is_arithmetic_v<T>) {
                if(argument == 0 || argument == 1)
                    return argument == 1;
            }
            return std::monostate{};
        },
        value
    );
}

inline ConfigValue CoerceAnimationValue(const EventData& value, WidgetPropertyType type) {
    ConfigValue converted;
    if(const auto* number = std::get_if<int>(&value)) {
        converted = *number;
    } else if(const auto* unsigned_number = std::get_if<uint32_t>(&value)) {
        using namespace eerie_leap::domain::ui_domain::models;
        const uint32_t minimum = type == WidgetPropertyType::ANIMATION_TYPE ? 0 : Animation::MIN_DURATION_MS;
        const uint32_t maximum = type == WidgetPropertyType::ANIMATION_TYPE
            ? static_cast<uint32_t>(Animation::Type::Rotation)
            : Animation::MAX_DURATION_MS;
        if(*unsigned_number >= minimum
            && *unsigned_number <= maximum
            && *unsigned_number <= static_cast<uint32_t>(std::numeric_limits<int>::max()))
            converted = static_cast<int>(*unsigned_number);
    }

    return WidgetPropertyValidator::IsValidAnimationValue(type, converted) ? converted : ConfigValue{};
}

inline ConfigValue CoerceOpacityValue(const EventData& value) {
    if(const auto* opacity = std::get_if<int>(&value); opacity != nullptr && *opacity >= 0 && *opacity <= 255)
        return *opacity;
    if(const auto* unsigned_opacity = std::get_if<uint32_t>(&value);
        unsigned_opacity != nullptr && *unsigned_opacity <= 255)
        return static_cast<int>(*unsigned_opacity);
    return std::monostate{};
}

// A color is its "#RRGGBBAA" text; a number stands for the packed 0xRRGGBB value, opaque.
inline ConfigValue CoerceColorValue(const EventData& value) {
    if(const auto* text = std::get_if<std::string>(&value))
        return std::pmr::string(*text, Mrm::GetExtPmr());

    uint32_t color_int = 0;
    if(const auto* packed = std::get_if<uint32_t>(&value)) {
        color_int = *packed;
    } else if(const auto* signed_packed = std::get_if<int>(&value)) {
        color_int = static_cast<uint32_t>(*signed_packed);
    } else if(const auto* fractional = std::get_if<float>(&value)) {
        color_int = static_cast<uint32_t>(*fractional);
    } else {
        return std::monostate{};
    }

    constexpr char HEX_DIGITS[] = "0123456789ABCDEF";
    char color_str[] = "#000000FF";
    for(size_t i = 6; i > 0; --i) {
        color_str[i] = HEX_DIGITS[color_int & 0xF];
        color_int >>= 4;
    }

    return ConfigValue(std::in_place_type<std::pmr::string>, color_str, sizeof(color_str) - 1, Mrm::GetExtPmr());
}

inline ConfigValue CoerceToAlternative(const EventData& value, size_t alternative) {
    return std::visit(
        [alternative](auto&& argument) -> ConfigValue {
            using T = std::decay_t<decltype(argument)>;

            constexpr bool IS_NUMBER = std::is_same_v<T, int>
                || std::is_same_v<T, uint32_t>
                || std::is_same_v<T, float>;

            switch(alternative) {
                case 1: // int
                    if constexpr(IS_NUMBER)
                        return static_cast<int>(argument);
                    else if constexpr(std::is_same_v<T, bool>)
                        return argument ? 1 : 0;
                    break;

                case 2: // double
                    if constexpr(IS_NUMBER)
                        return static_cast<double>(argument);
                    else if constexpr(std::is_same_v<T, bool>)
                        return argument ? 1.0 : 0.0;
                    break;

                case 3: // std::pmr::string
                    if constexpr(std::is_same_v<T, std::string>)
                        return std::pmr::string(argument.c_str(), Mrm::GetExtPmr());
                    break;

                case 4: // bool
                    if constexpr(std::is_same_v<T, bool>)
                        return argument;
                    else if constexpr(IS_NUMBER)
                        return argument != T{};
                    break;

                default:
                    break;
            }

            return std::monostate{};
        },
        value
    );
}

} // namespace detail

// An inbound value coerces to the alternative the property was registered with rather than to
// whatever the publisher happened to send, so a widget's members never change type under it.
// Monostate means the value cannot fill that property, and the event is dropped.
inline ConfigValue
CoerceToConfigValue(const EventData& value, size_t alternative, WidgetPropertyType type = WidgetPropertyType::NONE) {
    if(WidgetPropertyValidator::IsAnchorProperty(type))
        return detail::CoerceAnchorValue(value, type);

    if(WidgetPropertyValidator::IsLayoutProperty(type))
        return detail::CoerceLayoutValue(value);

    if(type == WidgetPropertyType::IS_ACTIVE
        || type == WidgetPropertyType::IS_VISIBLE
        || type == WidgetPropertyType::IS_ANIMATION_ACTIVE)
        return detail::CoerceFlagValue(value);

    if(WidgetPropertyValidator::IsAnimationProperty(type))
        return detail::CoerceAnimationValue(value, type);

    if(type == WidgetPropertyType::OPACITY)
        return detail::CoerceOpacityValue(value);

    if(WidgetPropertyValidator::IsColorProperty(type))
        return detail::CoerceColorValue(value);

    return detail::CoerceToAlternative(value, alternative);
}

inline EventData ToEventData(const ConfigValue& value) {
    return std::visit(
        [](auto&& argument) -> EventData {
            using T = std::decay_t<decltype(argument)>;

            if constexpr(std::is_same_v<T, int> || std::is_same_v<T, bool>)
                return argument;
            else if constexpr(std::is_same_v<T, double>)
                return static_cast<float>(argument);
            else if constexpr(std::is_same_v<T, std::pmr::string>)
                return std::string(argument);
            else
                return 0;
        },
        value
    );
}

} // namespace eerie_leap::views::widgets
