#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>

#include "domain/ui_domain/models/widget_composition.h"
#include "domain/ui_domain/models/widget_direction.h"
#include "domain/ui_domain/models/widget_fill_mode.h"
#include "domain/ui_domain/models/widget_property.h"
#include "domain/ui_domain/utilities/widget_property_validator.h"

#include "ui_configuration_validator.h"

namespace eerie_leap::domain::ui_domain::configuration::parsers {

using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::domain::ui_domain::utilities;

// Matches the screen count the persisted CBOR schema allows.
static constexpr std::size_t max_screen_count = 24;

enum class PropertyValueKind {
    Numeric,
    Boolean,
    Text,
    IntegerList
};

static PropertyValueKind GetPropertyValueKind(WidgetPropertyType type) {
    if(WidgetPropertyValidator::IsStructuralProperty(type))
        return PropertyValueKind::IntegerList;

    if(WidgetPropertyValidator::IsColorProperty(type))
        return PropertyValueKind::Text;

    switch(type) {
        case WidgetPropertyType::IS_ACTIVE:
        case WidgetPropertyType::IS_SMOOTHED:
        case WidgetPropertyType::IS_VISIBLE:
        case WidgetPropertyType::IS_ANIMATION_ACTIVE:
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

        case PropertyValueKind::IntegerList:
            return std::holds_alternative<std::pmr::vector<int>>(value);

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

    if(WidgetPropertyValidator::IsAppearanceProperty(type)
        && !WidgetPropertyValidator::IsValidAppearanceValue(type, value))
        return "Invalid value for property ID: " + std::to_string(static_cast<uint16_t>(type)) + ".";

    if(WidgetPropertyValidator::IsAnimationProperty(type)
        && !WidgetPropertyValidator::IsValidAnimationValue(type, value))
        return "Invalid value for property ID: " + std::to_string(static_cast<uint16_t>(type)) + ".";

    if(WidgetPropertyValidator::IsAnchorProperty(type)
        && !WidgetPropertyValidator::IsValidAnchorValue(type, value))
        return "Invalid anchor coordinate for property ID: " + std::to_string(static_cast<uint16_t>(type)) + ".";

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

void UiConfigurationValidator::Validate(const UiConfiguration& configuration, const ChildValidator& validate_children) {
    for(const auto& [type, value] : configuration.properties) {
        if(!IsValidUiPropertyType(type))
            InvalidUiConfiguration("Unknown UI property ID: " + std::to_string(static_cast<uint16_t>(type)) + ".");
    }

    ValidateScreenCount(configuration);
    ValidateScreenId(configuration);
    ValidateScreenType(configuration);
    ValidateScreenGrid(configuration);
    ValidateActiveScreenGroupId(configuration);

    ValidateScreens(configuration, validate_children);
}

void UiConfigurationValidator::Validate(const ScreenConfiguration& configuration, const ChildValidator& validate_children) {
    const auto& definitions = configuration.widget_configurations;
    if(definitions.size() > WidgetComposition::max_nodes)
        InvalidScreenConfiguration(configuration.id, "Composition exceeds the limit of 32 nodes.");

    // Bound all graph storage and work before allocating the index or traversal plan.
    size_t edge_count = 0;
    for(const auto& definition : definitions) {
        if(definition == nullptr)
            InvalidScreenConfiguration(configuration.id, "Composition contains a null widget definition.");
        auto it = definition->properties.find(WidgetPropertyType::CHILD_WIDGET_IDS);
        if(it == definition->properties.end())
            continue;
        const auto* ids = std::get_if<std::pmr::vector<int>>(&it->second);
        if(ids == nullptr)
            InvalidWidgetConfiguration(configuration.id, definition->id, "CHILD_WIDGET_IDS must be an integer list.");
        if(ids->size() > WidgetComposition::max_edges - edge_count) {
            const auto slot = WidgetComposition::max_edges - edge_count;
            InvalidWidgetConfiguration(configuration.id, definition->id,
                "Child index " + std::to_string(slot) + ", ID " + std::to_string((*ids)[slot])
                + ": composition exceeds the limit of 31 edges.");
        }
        edge_count += ids->size();
    }

    if(configuration.grid.width == 0 || configuration.grid.height == 0)
        InvalidScreenConfiguration(configuration.id, "Grid dimensions must be greater than 0.");
    ValidateWidgetType(configuration);
    ValidateWidgetProperties(configuration);
    ValidateWidgetBindings(configuration);

    auto* resource = definitions.get_allocator().resource();
    // Validation uses temporary ownership/traversal state, not a reusable composition.
    std::array<std::optional<size_t>, WidgetComposition::max_nodes> parents {};
    std::array<std::span<const int>, WidgetComposition::max_nodes> child_ids {};
    std::pmr::unordered_map<uint32_t, size_t> indices(resource);
    indices.reserve(definitions.size());
    for(size_t i = 0; i < definitions.size(); ++i) {
        if(!indices.emplace(definitions[i]->id, i).second)
            InvalidWidgetConfiguration(configuration.id, definitions[i]->id, "Screen cannot contain duplicate widget IDs.");
    }

    for(size_t i = 0; i < definitions.size(); ++i) {
        const auto& owner = *definitions[i];
        auto it = owner.properties.find(WidgetPropertyType::CHILD_WIDGET_IDS);
        if(it == owner.properties.end())
            continue;
        const auto& ids = std::get<std::pmr::vector<int>>(it->second);
        child_ids[i] = ids;
        for(size_t slot = 0; slot < ids.size(); ++slot) {
            const auto id = ids[slot];
            const auto edge = "Child index " + std::to_string(slot) + ", ID " + std::to_string(id) + ": ";
            if(id < 0 || static_cast<uint64_t>(id) > INT32_MAX)
                InvalidWidgetConfiguration(configuration.id, owner.id, edge + "ID must be between 0 and INT32_MAX.");
            auto child = indices.find(static_cast<uint32_t>(id));
            if(child == indices.end())
                InvalidWidgetConfiguration(configuration.id, owner.id, edge + "target does not exist in this screen.");
            if(child->second == i)
                InvalidWidgetConfiguration(configuration.id, owner.id, edge + "self-reference is not allowed.");
            auto& parent = parents[child->second];
            if(parent.has_value()) {
                if(*parent == i)
                    InvalidWidgetConfiguration(configuration.id, owner.id, edge + "duplicate reference in this owner.");
                InvalidWidgetConfiguration(configuration.id, owner.id,
                    edge + "already owned by widget " + std::to_string(definitions[*parent]->id) + ".");
            }
            parent = i;
        }
    }

    // Iterative DFS also visits components with no roots. A hostile cycle cannot
    // recurse through a Zephyr stack, and forward definition order does not affect depth.
    struct Visit { size_t index; size_t next_child; };
    std::array<Visit, WidgetComposition::max_nodes> stack;
    std::array<uint8_t, WidgetComposition::max_nodes> state {};
    std::array<size_t, WidgetComposition::max_nodes> height {};
    auto visit = [&](size_t start) {
        if(state[start] != 0)
            return;
        size_t stack_size = 1;
        stack[0] = { start, 0 };
        state[start] = 1;
        while(stack_size != 0) {
            auto& current = stack[stack_size - 1];
            const auto children = child_ids[current.index];
            if(current.next_child < children.size()) {
                const size_t slot = current.next_child++;
                const size_t child = indices.at(static_cast<uint32_t>(children[slot]));
                if(state[child] == 1) {
                    std::string path;
                    bool in_cycle = false;
                    for(size_t j = 0; j < stack_size; ++j) {
                        in_cycle |= stack[j].index == child;
                        if(in_cycle)
                            path += std::to_string(definitions[stack[j].index]->id) + " -> ";
                    }
                    path += std::to_string(definitions[child]->id);
                    InvalidWidgetConfiguration(configuration.id, definitions[current.index]->id,
                        "Child index " + std::to_string(slot) + ", ID " + std::to_string(definitions[child]->id)
                        + ": cycle " + path + ".");
                }
                if(state[child] == 0) {
                    state[child] = 1;
                    stack[stack_size++] = { child, 0 };
                }
            } else {
                size_t depth = 1;
                for(size_t slot = 0; slot < children.size(); ++slot) {
                    const auto child = indices.at(static_cast<uint32_t>(children[slot]));
                    depth = std::max(depth, height[child] + 1);
                    if(depth > WidgetComposition::max_depth)
                        InvalidWidgetConfiguration(configuration.id, definitions[current.index]->id,
                            "Child index " + std::to_string(slot) + ", ID " + std::to_string(definitions[child]->id)
                            + ": composition exceeds 8 levels of nesting (root is level 1).");
                }
                height[current.index] = depth;
                state[current.index] = 2;
                --stack_size;
            }
        }
    };
    for(size_t i = 0; i < definitions.size(); ++i)
        visit(i);

    const auto ownership = std::span(parents).first(definitions.size());
    ValidateWidgetSize(configuration, ownership);
    ValidateWidgetPosition(configuration, ownership);

    if(validate_children) {
        std::array<const WidgetConfiguration*, WidgetComposition::max_edges> children;
        for(size_t i = 0; i < definitions.size(); ++i) {
            const auto ids = child_ids[i];
            for(size_t slot = 0; slot < ids.size(); ++slot)
                children[slot] = definitions[indices.at(static_cast<uint32_t>(ids[slot]))].get();
            try {
                validate_children(*definitions[i], std::span(children).first(ids.size()));
            } catch(const std::invalid_argument& error) {
                InvalidWidgetConfiguration(configuration.id, definitions[i]->id, error.what());
            }
        }
    }
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

void UiConfigurationValidator::ValidateScreens(const UiConfiguration& configuration, const ChildValidator& validate_children) {
    for(const auto& screen_configuration : configuration.screen_configurations)
        Validate(*screen_configuration, validate_children);
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

void UiConfigurationValidator::ValidateWidgetSize(const ScreenConfiguration& screen_configuration,
    std::span<const std::optional<size_t>> parents) {
    for(size_t i = 0; i < screen_configuration.widget_configurations.size(); ++i) {
        if(parents[i].has_value())
            continue;
        const auto& widget_configuration = screen_configuration.widget_configurations[i];
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

void UiConfigurationValidator::ValidateWidgetPosition(const ScreenConfiguration& screen_configuration,
    std::span<const std::optional<size_t>> parents) {
    for(size_t i = 0; i < screen_configuration.widget_configurations.size(); ++i) {
        if(parents[i].has_value())
            continue;
        const auto& widget_configuration = screen_configuration.widget_configurations[i];
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

            if(WidgetPropertyValidator::IsStructuralProperty(binding.target))
                InvalidWidgetConfiguration(
                    screen_configuration.id,
                    widget_configuration->id,
                    "Binding cannot target a structural property; rebuild the screen to change child widgets."
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
