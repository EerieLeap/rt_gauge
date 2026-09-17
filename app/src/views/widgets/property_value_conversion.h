#pragma once

#include <cstddef>
#include <string>
#include <variant>

#include "domain/ui_domain/utilities/widget_property_validation.h"
#include "subsys/event_bus/event.h"

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/type/color.h"
#include "utilities/type/config_value.h"

namespace eerie_leap::views::widgets {

using eerie_leap::domain::ui_domain::models::WidgetPropertyType;
using eerie_leap::domain::ui_domain::utilities::IsWidgetColorProperty;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::utilities::type::Color;
using eerie_leap::utilities::type::ConfigValue;

// EventData and ConfigValue overlap but are not the same variant: the bus has no monostate and
// carries uint32/float, the configuration model carries double and PMR strings.
//
// An inbound value coerces to the alternative the property was registered with rather than to
// whatever the publisher happened to send, so a widget's members never change type under it.
// Monostate means the value cannot fill that property, and the event is dropped.
inline ConfigValue CoerceToConfigValue(
    const EventData& value, size_t alternative, WidgetPropertyType type = WidgetPropertyType::NONE) {
    if(type == WidgetPropertyType::IS_ACTIVE || type == WidgetPropertyType::IS_VISIBLE) {
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

    if(type == WidgetPropertyType::OPACITY) {
        if(const auto* opacity = std::get_if<int>(&value); opacity != nullptr && *opacity >= 0 && *opacity <= 255)
            return *opacity;
        if(const auto* opacity = std::get_if<uint32_t>(&value); opacity != nullptr && *opacity <= 255)
            return static_cast<int>(*opacity);
        return std::monostate{};
    }

    if(IsWidgetColorProperty(type)) {
        const auto* text = std::get_if<std::string>(&value);
        if(text != nullptr && Color::TryParse(*text).has_value())
            return std::pmr::string(*text, Mrm::GetExtPmr());
        return std::monostate{};
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
