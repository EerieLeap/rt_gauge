#include <stdexcept>
#include <string>
#include <utility>

#include "utilities/cbor/cbor_helpers.hpp"
#include "utilities/memory/memory_resource_manager.h"

#include "domain/ui_domain/models/widget_property.h"

#include "ui_configuration_validator.h"
#include "ui_configuration_cbor_parser.h"

namespace eerie_leap::domain::ui_domain::configuration::parsers {

using namespace eerie_memory;
using namespace eerie_leap::utilities::cbor;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;

using eerie_leap::domain::ui_domain::models::UiPropertyType;
using eerie_leap::domain::ui_domain::models::WidgetPropertyType;

namespace {

// zcbor_string is a non-owning view, so whatever it points at has to outlive the encoded config.
// Text taken from the model is safe because the caller holds the model across the encode.
void ToCborPropertyValue(CborPropertyValueType_r& prop, const ConfigValue& value) {
    std::visit([&](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;

        if constexpr (std::is_same_v<T, int>) {
            prop.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_c;
            prop.value = static_cast<int32_t>(arg);
        } else if constexpr (std::is_same_v<T, double>) {
            prop.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_float_c;
            prop.value = static_cast<double>(arg);
        } else if constexpr (std::is_same_v<T, std::pmr::string>) {
            prop.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_tstr_c;
            prop.value = CborHelpers::ToZcborString(arg);
        } else if constexpr (std::is_same_v<T, bool>) {
            prop.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_bool_c;
            prop.value = arg;
        } else if constexpr (std::is_same_v<T, std::pmr::vector<int>>) {
            prop.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_l_c;

            prop.value = std::pmr::vector<int32_t>(Mrm::GetExtPmr());
            for(const auto& item : arg)
                std::get<std::pmr::vector<int32_t>>(prop.value).push_back(item);
        } else if constexpr (std::is_same_v<T, std::pmr::vector<std::pmr::string>>) {
            prop.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_tstr_l_c;

            prop.value = std::pmr::vector<zcbor_string>(Mrm::GetExtPmr());
            for(const auto& item : arg)
                std::get<std::pmr::vector<zcbor_string>>(prop.value).push_back(CborHelpers::ToZcborString(item));
        } else if constexpr (std::is_same_v<T, std::pmr::unordered_map<std::pmr::string, std::pmr::string>>) {
            prop.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_map_c;

            prop.value = std::pmr::vector<map_tstrtstr>(Mrm::GetExtPmr());
            for(const auto& [key, item] : arg) {
                std::get<std::pmr::vector<map_tstrtstr>>(prop.value).push_back({
                    .tstrtstr_key = CborHelpers::ToZcborString(key),
                    .tstrtstr = CborHelpers::ToZcborString(item)
                });
            }
        } else {
            throw std::runtime_error("Unsupported property value type");
        }
    }, value);
}

ConfigValue FromCborPropertyValue(std::pmr::memory_resource* mr, const CborPropertyValueType_r& prop) {
    ConfigValue value;

    std::visit([&](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;

        if constexpr (std::is_same_v<T, int32_t>) {
            value = arg;
        } else if constexpr (std::is_same_v<T, double>) {
            value = static_cast<double>(arg);
        } else if constexpr (std::is_same_v<T, zcbor_string>) {
            value = CborHelpers::ToPmrString(mr, arg);
        } else if constexpr (std::is_same_v<T, bool>) {
            value = arg;
        } else if constexpr (std::is_same_v<T, std::pmr::vector<int32_t>>) {
            value = std::pmr::vector<int>(mr);
            for(const auto& item : arg)
                std::get<std::pmr::vector<int>>(value).push_back(item);
        } else if constexpr (std::is_same_v<T, std::pmr::vector<zcbor_string>>) {
            value = std::pmr::vector<std::pmr::string>(mr);
            for(const auto& item : arg)
                std::get<std::pmr::vector<std::pmr::string>>(value).push_back(CborHelpers::ToPmrString(mr, item));
        } else if constexpr (std::is_same_v<T, std::pmr::vector<map_tstrtstr>>) {
            value = std::pmr::unordered_map<std::pmr::string, std::pmr::string>(mr);
            for(const auto& item : arg) {
                std::get<std::pmr::unordered_map<std::pmr::string, std::pmr::string>>(value).insert({
                    CborHelpers::ToPmrString(mr, item.tstrtstr_key),
                    CborHelpers::ToPmrString(mr, item.tstrtstr)
                });
            }
        } else {
            throw std::runtime_error("Unsupported property value type");
        }
    }, prop.value);

    return value;
}

UiPropertyType FromCborUiPropertyType(uint32_t type) {
    // Validate before narrowing to the enum's uint16_t underlying type.
    if(type == static_cast<uint32_t>(UiPropertyType::NONE)
        || type >= static_cast<uint32_t>(UiPropertyType::COUNT))
        throw std::invalid_argument("Invalid UI configuration. Unknown UI property ID: " + std::to_string(type));

    return static_cast<UiPropertyType>(type);
}

void CborPropertyValueTypeToUiValueType(
    std::pmr::memory_resource* mr,
    std::pmr::unordered_map<UiPropertyType, ConfigValue>& properties,
    const CborPropertiesConfig& properties_config) {

    for(const auto& property : properties_config.CborPropertyValueType_m) {
        auto type = FromCborUiPropertyType(property.CborPropertyValueType_m_key);
        auto [it, inserted] = properties.emplace(type, FromCborPropertyValue(mr, property.CborPropertyValueType_m));
        if(!inserted)
            throw std::invalid_argument("Invalid UI configuration. Duplicate property ID: "
                + std::to_string(property.CborPropertyValueType_m_key));
    }
}

WidgetPropertyType FromCborWidgetPropertyType(uint32_t type) {
    // Validate before narrowing to the enum's uint16_t underlying type.
    if(type == static_cast<uint32_t>(WidgetPropertyType::NONE)
        || type >= static_cast<uint32_t>(WidgetPropertyType::COUNT))
        throw std::invalid_argument("Invalid UI configuration. Unknown property ID: " + std::to_string(type));

    return static_cast<WidgetPropertyType>(type);
}

void CborPropertyValueTypeToWidgetValueType(
    std::pmr::memory_resource* mr,
    std::pmr::unordered_map<WidgetPropertyType, ConfigValue>& properties,
    const CborPropertiesConfig& properties_config) {

    for(const auto& property : properties_config.CborPropertyValueType_m) {
        auto type = FromCborWidgetPropertyType(property.CborPropertyValueType_m_key);
        auto [it, inserted] = properties.emplace(type, FromCborPropertyValue(mr, property.CborPropertyValueType_m));
        if(!inserted)
            throw std::invalid_argument("Invalid UI configuration. Duplicate property ID: "
                + std::to_string(property.CborPropertyValueType_m_key));
    }
}

void ToCborBindings(std::pmr::vector<CborPropertyBinding>& bindings_config, const std::pmr::vector<PropertyBinding>& bindings) {
    for(const auto& binding : bindings) {
        CborPropertyBinding binding_config(std::allocator_arg, Mrm::GetExtPmr());

        binding_config.target = static_cast<uint32_t>(binding.target);
        binding_config.channel = static_cast<uint32_t>(binding.channel);
        binding_config.event_type = binding.event_type;
        binding_config.payload_key = binding.payload_key;
        binding_config.direction = static_cast<uint32_t>(binding.direction);
        binding_config.outbound_event_type = binding.outbound_event_type;
        binding_config.selector_key = binding.selector_key;
        binding_config.has_selector = binding.HasSelector();

        // The schema has no null, so an absent selector still has to encode some value.
        if(binding_config.has_selector)
            ToCborPropertyValue(binding_config.selector_value, binding.selector_value);
        else
            ToCborPropertyValue(binding_config.selector_value, ConfigValue { 0 });

        bindings_config.push_back(std::move(binding_config));
    }
}

void FromCborBindings(
    std::pmr::memory_resource* mr,
    std::pmr::vector<PropertyBinding>& bindings,
    const std::pmr::vector<CborPropertyBinding>& bindings_config) {

    for(const auto& binding_config : bindings_config) {
        PropertyBinding binding;

        binding.target = FromCborWidgetPropertyType(binding_config.target);
        binding.channel = static_cast<EventChannelId>(binding_config.channel);
        binding.event_type = binding_config.event_type;
        binding.payload_key = binding_config.payload_key;
        binding.direction = static_cast<PropertyBindingDirection>(binding_config.direction);
        binding.outbound_event_type = binding_config.outbound_event_type;
        binding.selector_key = binding_config.selector_key;

        if(binding_config.has_selector)
            binding.selector_value = FromCborPropertyValue(mr, binding_config.selector_value);

        bindings.push_back(std::move(binding));
    }
}

template<typename T>
void ValueTypeToCborPropertyValueType(
    CborPropertiesConfig& properties_config,
    const std::pmr::unordered_map<T, ConfigValue>& properties) {

    for(const auto& [type, value] : properties) {
        CborPropertiesConfig_CborPropertyValueType_m property(std::allocator_arg, Mrm::GetExtPmr());
        property.CborPropertyValueType_m_key = static_cast<uint32_t>(type);
        ToCborPropertyValue(property.CborPropertyValueType_m, value);
        properties_config.CborPropertyValueType_m.push_back(std::move(property));
    }
}

} // namespace

UiConfigurationCborParser::UiConfigurationCborParser(UiConfigurationValidator::ChildValidator validate_children)
    : validate_children_(std::move(validate_children)) {}

pmr_unique_ptr<CborUiConfig> UiConfigurationCborParser::Serialize(const UiConfiguration& configuration) {
    UiConfigurationValidator::Validate(configuration, validate_children_);

    auto config = make_unique_pmr<CborUiConfig>(Mrm::GetExtPmr());

    config->version = configuration_version;
    config->active_screen_group_id = configuration.active_screen_group_id;

    config->properties_present = configuration.properties.size() > 0;
    if(configuration.properties.size() > 0)
        ValueTypeToCborPropertyValueType<UiPropertyType>(config->properties, configuration.properties);

    config->CborScreenConfig_m.clear();
    for(int i = 0; i < configuration.screen_configurations.size(); i++) {
        CborScreenConfig screen_config(std::allocator_arg, Mrm::GetExtPmr());;

        screen_config.id = configuration.screen_configurations[i]->id;
        screen_config.group_id = configuration.screen_configurations[i]->screen_group_id;
        screen_config.type = static_cast<uint32_t>(configuration.screen_configurations[i]->type);
        screen_config.z_index = configuration.screen_configurations[i]->z_index;
        screen_config.is_visible = configuration.screen_configurations[i]->is_visible;
        screen_config.is_overlay = configuration.screen_configurations[i]->is_overlay;
        screen_config.grid.snap_enabled = configuration.screen_configurations[i]->grid.snap_enabled;
        screen_config.grid.width = configuration.screen_configurations[i]->grid.width;
        screen_config.grid.height = configuration.screen_configurations[i]->grid.height;
        screen_config.grid.spacing_px = configuration.screen_configurations[i]->grid.spacing_px;

        screen_config.CborWidgetConfig_m.clear();
        for(int j = 0; j < configuration.screen_configurations[i]->widget_configurations.size(); j++) {
            CborWidgetConfig widget_config(std::allocator_arg, Mrm::GetExtPmr());;

            widget_config.id = configuration.screen_configurations[i]->widget_configurations[j]->id;
            widget_config.type = static_cast<uint32_t>(configuration.screen_configurations[i]->widget_configurations[j]->type);
            widget_config.position.x = configuration.screen_configurations[i]->widget_configurations[j]->position_grid.x;
            widget_config.position.y = configuration.screen_configurations[i]->widget_configurations[j]->position_grid.y;
            widget_config.size.width = configuration.screen_configurations[i]->widget_configurations[j]->size_grid.width;
            widget_config.size.height = configuration.screen_configurations[i]->widget_configurations[j]->size_grid.height;
            widget_config.z_index = configuration.screen_configurations[i]->widget_configurations[j]->z_index;

            widget_config.properties_present = configuration.screen_configurations[i]->widget_configurations[j]->properties.size() > 0;
            if(widget_config.properties_present)
                ValueTypeToCborPropertyValueType<WidgetPropertyType>(widget_config.properties, configuration.screen_configurations[i]->widget_configurations[j]->properties);

            widget_config.CborPropertyBinding_m.clear();
            ToCborBindings(
                widget_config.CborPropertyBinding_m,
                configuration.screen_configurations[i]->widget_configurations[j]->bindings);

            screen_config.CborWidgetConfig_m.push_back(std::move(widget_config));
        }

        config->CborScreenConfig_m.push_back(std::move(screen_config));
    }

    return config;
}

pmr_unique_ptr<UiConfiguration> UiConfigurationCborParser::Deserialize(
    std::pmr::memory_resource* mr,
    const CborUiConfig& config) {

    // Nothing migrates, so a mismatch lets UiConfigurationManager fall through to its default.
    if(config.version != configuration_version)
        throw std::invalid_argument(
            "Invalid UI configuration. Unsupported version: "
            + std::to_string(config.version)
            + ". Expected: "
            + std::to_string(configuration_version)
            + ".");

    auto configuration = make_unique_pmr<UiConfiguration>(mr);

    configuration->active_screen_group_id = config.active_screen_group_id;

    if(config.properties_present)
        CborPropertyValueTypeToUiValueType(mr, configuration->properties, config.properties);

    for(int i = 0; i < config.CborScreenConfig_m.size(); i++) {
        auto screen_configuration = make_shared_pmr<ScreenConfiguration>(mr);
        screen_configuration->id = config.CborScreenConfig_m[i].id;
        screen_configuration->screen_group_id = config.CborScreenConfig_m[i].group_id;
        screen_configuration->type = static_cast<ScreenType>(config.CborScreenConfig_m[i].type);
        screen_configuration->z_index = config.CborScreenConfig_m[i].z_index;
        screen_configuration->is_visible = config.CborScreenConfig_m[i].is_visible;
        screen_configuration->is_overlay = config.CborScreenConfig_m[i].is_overlay;

        screen_configuration->grid.snap_enabled = config.CborScreenConfig_m[i].grid.snap_enabled;
        screen_configuration->grid.width = config.CborScreenConfig_m[i].grid.width;
        screen_configuration->grid.height = config.CborScreenConfig_m[i].grid.height;
        screen_configuration->grid.spacing_px = config.CborScreenConfig_m[i].grid.spacing_px;

        for(int j = 0; j < config.CborScreenConfig_m[i].CborWidgetConfig_m.size(); j++) {
            auto widget_configuration = make_shared_pmr<WidgetConfiguration>(mr);
            widget_configuration->type = static_cast<WidgetType>(config.CborScreenConfig_m[i].CborWidgetConfig_m[j].type);
            widget_configuration->id = config.CborScreenConfig_m[i].CborWidgetConfig_m[j].id;
            widget_configuration->position_grid.x = config.CborScreenConfig_m[i].CborWidgetConfig_m[j].position.x;
            widget_configuration->position_grid.y = config.CborScreenConfig_m[i].CborWidgetConfig_m[j].position.y;
            widget_configuration->size_grid.width = config.CborScreenConfig_m[i].CborWidgetConfig_m[j].size.width;
            widget_configuration->size_grid.height = config.CborScreenConfig_m[i].CborWidgetConfig_m[j].size.height;
            widget_configuration->z_index = config.CborScreenConfig_m[i].CborWidgetConfig_m[j].z_index;

            if(config.CborScreenConfig_m[i].CborWidgetConfig_m[j].properties_present)
                CborPropertyValueTypeToWidgetValueType(mr, widget_configuration->properties, config.CborScreenConfig_m[i].CborWidgetConfig_m[j].properties);

            FromCborBindings(
                mr,
                widget_configuration->bindings,
                config.CborScreenConfig_m[i].CborWidgetConfig_m[j].CborPropertyBinding_m);

            screen_configuration->AddWidget(std::move(widget_configuration));
        }

        configuration->screen_configurations.push_back(std::move(screen_configuration));
    }

    UiConfigurationValidator::Validate(*configuration, validate_children_);

    return configuration;
}

} // namespace eerie_leap::domain::ui_domain::configuration::parsers
