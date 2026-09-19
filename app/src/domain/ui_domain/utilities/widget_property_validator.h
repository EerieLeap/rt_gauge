#pragma once

#include <array>
#include <cstddef>
#include <optional>

#include "domain/ui_domain/models/animation.h"
#include "domain/ui_domain/models/widget_property.h"
#include "utilities/type/color.h"
#include "utilities/type/config_value.h"

namespace eerie_leap::domain::ui_domain::utilities {

using eerie_leap::domain::ui_domain::models::WidgetPropertyType;
using eerie_leap::utilities::type::Color;
using eerie_leap::utilities::type::ConfigValue;

class WidgetPropertyValidator {
public:
    static constexpr bool IsAnimationProperty(WidgetPropertyType type) {
        return type == WidgetPropertyType::ANIMATION_TYPE
            || type == WidgetPropertyType::IS_ANIMATION_ACTIVE
            || type == WidgetPropertyType::ANIMATION_DURATION_MS;
    }

    static bool IsValidAnimationValue(WidgetPropertyType type, const ConfigValue& value) {
        using namespace eerie_leap::domain::ui_domain::models;

        if(type == WidgetPropertyType::IS_ANIMATION_ACTIVE)
            return std::holds_alternative<bool>(value);

        const auto* number = std::get_if<int>(&value);
        if(number == nullptr)
            return false;

        if(type == WidgetPropertyType::ANIMATION_TYPE)
            return *number == static_cast<int>(Animation::Type::None)
                || *number == static_cast<int>(Animation::Type::Blinking)
                || *number == static_cast<int>(Animation::Type::Rotation);

        if(type == WidgetPropertyType::ANIMATION_DURATION_MS)
            return *number >= Animation::MIN_DURATION_MS && *number <= Animation::MAX_DURATION_MS;

        return false;
    }

    static constexpr bool IsManagementProperty(WidgetPropertyType type) {
        return type == WidgetPropertyType::IS_ACTIVE
            || type == WidgetPropertyType::IS_VISIBLE
            || type == WidgetPropertyType::OPACITY;
    }

    inline static constexpr std::array color_properties {
        WidgetPropertyType::COLOR_PRIMARY_ACTIVE,
        WidgetPropertyType::COLOR_PRIMARY_INACTIVE,
        WidgetPropertyType::COLOR_SECONDARY_ACTIVE,
        WidgetPropertyType::COLOR_SECONDARY_INACTIVE,
        WidgetPropertyType::COLOR_TERTIARY_ACTIVE,
        WidgetPropertyType::COLOR_TERTIARY_INACTIVE
    };

    static constexpr std::optional<std::size_t> GetColorPropertyIndex(WidgetPropertyType type) {
        for(std::size_t index = 0; index < color_properties.size(); ++index) {
            if(color_properties[index] == type)
                return index;
        }
        return std::nullopt;
    }

    static constexpr bool IsColorProperty(WidgetPropertyType type) {
        return GetColorPropertyIndex(type).has_value();
    }

    static constexpr bool IsAppearanceProperty(WidgetPropertyType type) {
        return IsManagementProperty(type) || IsColorProperty(type);
    }

    static bool IsValidAppearanceValue(WidgetPropertyType type, const ConfigValue& value) {
        if(type == WidgetPropertyType::IS_ACTIVE || type == WidgetPropertyType::IS_VISIBLE)
            return std::holds_alternative<bool>(value);

        if(IsColorProperty(type)) {
            const auto* text = std::get_if<std::pmr::string>(&value);
            return text != nullptr && Color::TryParse(*text).has_value();
        }

        if(type == WidgetPropertyType::OPACITY) {
            const auto* opacity = std::get_if<int>(&value);
            return opacity != nullptr && *opacity >= 0 && *opacity <= 255;
        }

        return false;
    }
};

} // namespace eerie_leap::domain::ui_domain::utilities
