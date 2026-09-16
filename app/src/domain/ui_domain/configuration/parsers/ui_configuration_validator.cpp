#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <variant>

#include "domain/ui_domain/models/widget_direction.h"
#include "domain/ui_domain/models/widget_fill_mode.h"
#include "domain/ui_domain/models/widget_property.h"

#include "ui_configuration_validator.h"

namespace eerie_leap::domain::ui_domain::configuration::parsers {

using namespace eerie_leap::domain::ui_domain::models;

// Matches the screen count the persisted CBOR schema allows.
static constexpr std::size_t max_screen_count = 24;

enum class PropertyValueKind {
    Numeric,
    Boolean,
    Text
};

static PropertyValueKind GetPropertyValueKind(WidgetPropertyType type) {
    switch(type) {
        case WidgetPropertyType::IS_ACTIVE:
        case WidgetPropertyType::IS_SMOOTHED:
        case WidgetPropertyType::IS_VISIBLE:
            return PropertyValueKind::Boolean;

        case WidgetPropertyType::LABEL:
        case WidgetPropertyType::FILE_PATH:
        case WidgetPropertyType::SETTING_ID:
        case WidgetPropertyType::UNIT:
            return PropertyValueKind::Text;

        default:
            return PropertyValueKind::Numeric;
    }
}

static bool HoldsPropertyValueKind(const ConfigValue& value, PropertyValueKind kind) {
    switch(kind) {
        case PropertyValueKind::Boolean:
            return std::holds_alternative<bool>(value);

        case PropertyValueKind::Text:
            return std::holds_alternative<std::pmr::string>(value);

        // ConfigValueAs converts between int and double.
        case PropertyValueKind::Numeric:
            return std::holds_alternative<int>(value) || std::holds_alternative<double>(value);
    }

    return false;
}

static bool IsValidFillMode(const ConfigValue& value) {
    if(!std::holds_alternative<int>(value) && !std::holds_alternative<double>(value))
        return false;
    double val = std::holds_alternative<int>(value) ? std::get<int>(value) : std::get<double>(value);
    return val == static_cast<double>(WidgetFillMode::Filled) || val == static_cast<double>(WidgetFillMode::Outline);
}

static bool IsValidDirection(const ConfigValue& value) {
    if(!std::holds_alternative<int>(value) && !std::holds_alternative<double>(value))
        return false;
    double val = std::holds_alternative<int>(value) ? std::get<int>(value) : std::get<double>(value);
    return val >= 1.0 && val <= 4.0 && val == std::floor(val);
}

static std::string GetWidgetPropertyValidationError(WidgetPropertyType type, const ConfigValue& value) {
    if(!IsValidWidgetPropertyType(type))
        return "Unknown property ID: " + std::to_string(static_cast<uint16_t>(type)) + ".";

    if(!HoldsPropertyValueKind(value, GetPropertyValueKind(type)))
        return "Invalid value type for property ID: " + std::to_string(static_cast<uint16_t>(type)) + ".";

    if(type == WidgetPropertyType::FILL_MODE && !IsValidFillMode(value))
        return "Invalid value for property 'FILL_MODE'.";

    if(type == WidgetPropertyType::DIRECTION && !IsValidDirection(value))
        return "Invalid value for property 'DIRECTION'.";

    return {};
}

static bool IsValidEventChannelId(EventChannelId channel) {
    switch(channel) {
        case EventChannelId::Sensors:
        case EventChannelId::Logging:
        case EventChannelId::Navigation:
        case EventChannelId::Settings:
            return true;

        default:
            return false;
    }
}

static bool IsValidBindingDirection(PropertyBindingDirection direction) {
    switch(direction) {
        case PropertyBindingDirection::In:
        case PropertyBindingDirection::Out:
        case PropertyBindingDirection::InOut:
            return true;

        default:
            return false;
    }
}

// A selector is compared against a uint32 already in the payload: text is hashed, an integer is
// used as it stands. Nothing else reduces to that comparison.
static bool IsComparableSelectorValue(const ConfigValue& value) {
    return std::holds_alternative<std::pmr::string>(value) || std::holds_alternative<int>(value);
}

static void InvalidUiConfiguration(std::string_view message) {
    throw std::invalid_argument(
        "Invalid UI configuration. "
        + std::string(message));
}

static void InvalidScreenConfiguration(uint32_t screen_id, std::string_view message) {
    throw std::invalid_argument(
        "Invalid UI Screen configuration. Screen ID: "
        + std::to_string(screen_id)
        + ". "
        + std::string(message));
}

static void InvalidWidgetConfiguration(uint32_t screen_id, uint32_t widget_id, std::string_view message) {
    throw std::invalid_argument(
        "Invalid UI Widget configuration. Screen ID: "
        + std::to_string(screen_id)
        + ", Widget ID: "
        + std::to_string(widget_id)
        + ". "
        + std::string(message));
}

void UiConfigurationValidator::Validate(const UiConfiguration& configuration) {
    for(const auto& [type, value] : configuration.properties) {
        if(!IsValidUiPropertyType(type))
            InvalidUiConfiguration("Unknown UI property ID: " + std::to_string(static_cast<uint16_t>(type)) + ".");
    }

    ValidateScreenCount(configuration);
    ValidateScreenId(configuration);
    ValidateScreenType(configuration);
    ValidateScreenGrid(configuration);
    ValidateActiveScreenGroupId(configuration);

    ValidateScreens(configuration);
}

void UiConfigurationValidator::ValidateScreenCount(const UiConfiguration& configuration) {
    if(configuration.screen_configurations.size() > max_screen_count)
        InvalidUiConfiguration("Configuration cannot contain more than " + std::to_string(max_screen_count) + " screens.");
}

void UiConfigurationValidator::ValidateScreenId(const UiConfiguration& configuration) {
    std::unordered_set<uint32_t> screen_ids;

    for(const auto& screen_configuration : configuration.screen_configurations) {
        if(screen_ids.contains(screen_configuration->id))
            InvalidScreenConfiguration(screen_configuration->id, "Configuration cannot contain duplicate screen IDs.");

        screen_ids.insert(screen_configuration->id);
    }
}

void UiConfigurationValidator::ValidateScreenType(const UiConfiguration& configuration) {
    for(const auto& screen_configuration : configuration.screen_configurations) {
        switch(screen_configuration->type) {
            case ScreenType::System:
            case ScreenType::Gauge:
            case ScreenType::Settings:
            case ScreenType::Popup:
                break;

            default:
                InvalidScreenConfiguration(screen_configuration->id, "Invalid screen type.");
        }
    }
}

void UiConfigurationValidator::ValidateScreenGrid(const UiConfiguration& configuration) {
    for(const auto& screen_configuration : configuration.screen_configurations) {
        if(screen_configuration->grid.width == 0)
            InvalidScreenConfiguration(screen_configuration->id, "Grid width must be greater than 0.");

        if(screen_configuration->grid.height == 0)
            InvalidScreenConfiguration(screen_configuration->id, "Grid height must be greater than 0.");
    }
}

void UiConfigurationValidator::ValidateActiveScreenGroupId(const UiConfiguration& configuration) {
    // A configuration without screens is the default one created on first boot.
    if(configuration.screen_configurations.empty())
        return;

    for(const auto& screen_configuration : configuration.screen_configurations) {
        if(screen_configuration->screen_group_id == configuration.active_screen_group_id)
            return;
    }

    InvalidUiConfiguration("Active screen group ID does not match any screen.");
}

void UiConfigurationValidator::ValidateScreens(const UiConfiguration& configuration) {
    for(const auto& screen_configuration : configuration.screen_configurations)
        ValidateWidgets(*screen_configuration);
}

void UiConfigurationValidator::ValidateWidgets(const ScreenConfiguration& screen_configuration) {
    ValidateWidgetId(screen_configuration);
    ValidateWidgetType(screen_configuration);
    ValidateWidgetSize(screen_configuration);
    ValidateWidgetPosition(screen_configuration);
    ValidateWidgetProperties(screen_configuration);
    ValidateWidgetBindings(screen_configuration);
}

void UiConfigurationValidator::ValidateWidgetId(const ScreenConfiguration& screen_configuration) {
    std::unordered_set<uint32_t> widget_ids;

    for(const auto& widget_configuration : screen_configuration.widget_configurations) {
        if(widget_ids.contains(widget_configuration->id))
            InvalidWidgetConfiguration(
                screen_configuration.id,
                widget_configuration->id,
                "Screen cannot contain duplicate widget IDs."
            );

        widget_ids.insert(widget_configuration->id);
    }
}

void UiConfigurationValidator::ValidateWidgetType(const ScreenConfiguration& screen_configuration) {
    for(const auto& widget_configuration : screen_configuration.widget_configurations) {
        switch(widget_configuration->type) {
            case WidgetType::BasicIcon:
            case WidgetType::BasicArcIcon:
            case WidgetType::IndicatorArcFill:
            case WidgetType::IndicatorDigital:
            case WidgetType::IndicatorHorizontalChart:
            case WidgetType::IndicatorSegmentArc:
            case WidgetType::IndicatorDial:
            case WidgetType::IndicatorBar:
            case WidgetType::IndicatorSetting:
            case WidgetType::ControlSlider:
            case WidgetType::ControlToggle:
            case WidgetType::ControlButton:
                break;

            default:
                InvalidWidgetConfiguration(
                    screen_configuration.id,
                    widget_configuration->id,
                    "Invalid widget type."
                );
        }
    }
}

void UiConfigurationValidator::ValidateWidgetSize(const ScreenConfiguration& screen_configuration) {
    for(const auto& widget_configuration : screen_configuration.widget_configurations) {
        if(widget_configuration->size_grid.width == 0 || widget_configuration->size_grid.height == 0)
            InvalidWidgetConfiguration(
                screen_configuration.id,
                widget_configuration->id,
                "Widget size must be greater than 0."
            );

        if(widget_configuration->size_grid.width > screen_configuration.grid.width)
            InvalidWidgetConfiguration(
                screen_configuration.id,
                widget_configuration->id,
                "Widget width cannot exceed the screen grid width."
            );

        if(widget_configuration->size_grid.height > screen_configuration.grid.height)
            InvalidWidgetConfiguration(
                screen_configuration.id,
                widget_configuration->id,
                "Widget height cannot exceed the screen grid height."
            );
    }
}

void UiConfigurationValidator::ValidateWidgetPosition(const ScreenConfiguration& screen_configuration) {
    for(const auto& widget_configuration : screen_configuration.widget_configurations) {
        if(widget_configuration->position_grid.x < 0 || widget_configuration->position_grid.y < 0)
            InvalidWidgetConfiguration(
                screen_configuration.id,
                widget_configuration->id,
                "Widget position cannot be negative."
            );

        if(static_cast<uint32_t>(widget_configuration->position_grid.x) >= screen_configuration.grid.width)
            InvalidWidgetConfiguration(
                screen_configuration.id,
                widget_configuration->id,
                "Widget position X must be within the screen grid."
            );

        if(static_cast<uint32_t>(widget_configuration->position_grid.y) >= screen_configuration.grid.height)
            InvalidWidgetConfiguration(
                screen_configuration.id,
                widget_configuration->id,
                "Widget position Y must be within the screen grid."
            );
    }
}

void UiConfigurationValidator::ValidateWidgetProperties(const ScreenConfiguration& screen_configuration) {
    for(const auto& widget_configuration : screen_configuration.widget_configurations) {
        for(const auto& [property_type, value] : widget_configuration->properties) {
            if(auto error = GetWidgetPropertyValidationError(property_type, value); !error.empty())
                InvalidWidgetConfiguration(
                    screen_configuration.id,
                    widget_configuration->id,
                    error
                );
        }
    }
}

// Whether the target property means anything for this widget type is not checked here: only the
// widget classes know what they support, and they warn about the rest when they configure.
void UiConfigurationValidator::ValidateWidgetBindings(const ScreenConfiguration& screen_configuration) {
    for(const auto& widget_configuration : screen_configuration.widget_configurations) {
        for(const auto& binding : widget_configuration->bindings) {
            if(!IsValidEventChannelId(binding.channel))
                InvalidWidgetConfiguration(
                    screen_configuration.id,
                    widget_configuration->id,
                    "Binding names an unknown event channel."
                );

            if(!IsValidBindingDirection(binding.direction))
                InvalidWidgetConfiguration(
                    screen_configuration.id,
                    widget_configuration->id,
                    "Binding has an invalid direction."
                );

            if(binding.target == WidgetPropertyType::NONE)
                InvalidWidgetConfiguration(
                    screen_configuration.id,
                    widget_configuration->id,
                    "Binding must name a target property."
                );

            if(!IsValidWidgetPropertyType(binding.target))
                InvalidWidgetConfiguration(
                    screen_configuration.id,
                    widget_configuration->id,
                    "Binding names an unknown target property."
                );

            if(binding.HasSelector() && !IsComparableSelectorValue(binding.selector_value))
                InvalidWidgetConfiguration(
                    screen_configuration.id,
                    widget_configuration->id,
                    "Binding selector value must be text or an integer."
                );
        }
    }
}

} // namespace eerie_leap::domain::ui_domain::configuration::parsers
