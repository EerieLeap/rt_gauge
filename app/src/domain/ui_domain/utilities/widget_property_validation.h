#pragma once

#include "domain/ui_domain/models/widget_property.h"
#include "utilities/type/color.h"
#include "utilities/type/config_value.h"

namespace eerie_leap::domain::ui_domain::utilities {

using eerie_leap::domain::ui_domain::models::WidgetPropertyType;
using eerie_leap::utilities::type::Color;
using eerie_leap::utilities::type::ConfigValue;

constexpr bool IsWidgetManagementProperty(WidgetPropertyType type) {
    return type == WidgetPropertyType::IS_ACTIVE
        || type == WidgetPropertyType::IS_VISIBLE
        || type == WidgetPropertyType::OPACITY;
}

constexpr bool IsWidgetColorProperty(WidgetPropertyType type) {
    return type >= WidgetPropertyType::COLOR_PRIMARY_ACTIVE && type <= WidgetPropertyType::COLOR_TERTIARY_INACTIVE;
}

inline bool IsValidWidgetAppearanceValue(WidgetPropertyType type, const ConfigValue& value) {
    if(type == WidgetPropertyType::IS_ACTIVE || type == WidgetPropertyType::IS_VISIBLE)
        return std::holds_alternative<bool>(value);

    if(IsWidgetColorProperty(type)) {
        const auto* text = std::get_if<std::pmr::string>(&value);
        return text != nullptr && Color::TryParse(*text).has_value();
    }

    if(type == WidgetPropertyType::OPACITY) {
        const auto* opacity = std::get_if<int>(&value);
        return opacity != nullptr && *opacity >= 0 && *opacity <= 255;
    }

    return true;
}

} // namespace eerie_leap::domain::ui_domain::utilities
