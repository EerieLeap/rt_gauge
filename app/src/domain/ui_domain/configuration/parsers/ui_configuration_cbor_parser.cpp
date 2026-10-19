#include <stdexcept>
#include <string>
#include <utility>

#include "utilities/cbor/cbor_helpers.hpp"
#include "utilities/memory/memory_resource_manager.h"

#include "domain/ui_domain/models/widget_property.h"

#include "ui_configuration_cbor_parser.h"
#include "ui_configuration_validator.h"

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
    std::visit(
        [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;

            if constexpr(std::is_same_v<T, int>) {
                prop.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_c;
                prop.value = static_cast<int32_t>(arg);
            } else if constexpr(std::is_same_v<T, double>) {
                prop.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_float_c;
                prop.value = static_cast<double>(arg);
            } else if constexpr(std::is_same_v<T, std::pmr::string>) {
                prop.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_tstr_c;
                prop.value = CborHelpers::ToZcborString(arg);
            } else if constexpr(std::is_same_v<T, bool>) {
                prop.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_bool_c;
                prop.value = arg;
            } else if constexpr(std::is_same_v<T, std::pmr::vector<int>>) {
                prop.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_int_l_c;

                prop.value = std::pmr::vector<int32_t>(Mrm::GetExtPmr());
                for(const auto& item : arg)
                    std::get<std::pmr::vector<int32_t>>(prop.value).push_back(item);
            } else if constexpr(std::is_same_v<T, std::pmr::vector<std::pmr::string>>) {
                prop.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_tstr_l_c;

                prop.value = std::pmr::vector<zcbor_string>(Mrm::GetExtPmr());
                for(const auto& item : arg)
                    std::get<std::pmr::vector<zcbor_string>>(prop.value).push_back(CborHelpers::ToZcborString(item));
            } else if constexpr(std::is_same_v<T, std::pmr::unordered_map<std::pmr::string, std::pmr::string>>) {
                prop.CborPropertyValueType_choice = CborPropertyValueType_r::CborPropertyValueType_map_c;

                prop.value = std::pmr::vector<map_tstrtstr>(Mrm::GetExtPmr());
                for(const auto& [key, item] : arg) {
                    std::get<std::pmr::vector<map_tstrtstr>>(prop.value)
                        .push_back(
                            { .tstrtstr_key = CborHelpers::ToZcborString(key),
                                .tstrtstr = CborHelpers::ToZcborString(item) }
                        );
                }
            } else {
                throw std::runtime_error("Unsupported property value type");
            }
        },
        value
    );
}

ConfigValue FromCborPropertyValue(std::pmr::memory_resource* mr, const CborPropertyValueType_r& prop) {
    ConfigValue value;

    std::visit(
        [&](auto&& arg) {
            using T = std::decay_t<decltype(arg)>;

            if constexpr(std::is_same_v<T, int32_t> || std::is_same_v<T, bool>) {
                value = arg;
            } else if constexpr(std::is_same_v<T, double>) {
                value = static_cast<double>(arg);
            } else if constexpr(std::is_same_v<T, zcbor_string>) {
                value = CborHelpers::ToPmrString(mr, arg);
            } else if constexpr(std::is_same_v<T, std::pmr::vector<int32_t>>) {
                value = std::pmr::vector<int>(mr);
                for(const auto& item : arg)
                    std::get<std::pmr::vector<int>>(value).push_back(item);
            } else if constexpr(std::is_same_v<T, std::pmr::vector<zcbor_string>>) {
                value = std::pmr::vector<std::pmr::string>(mr);
                for(const auto& item : arg)
                    std::get<std::pmr::vector<std::pmr::string>>(value).push_back(CborHelpers::ToPmrString(mr, item));
            } else if constexpr(std::is_same_v<T, std::pmr::vector<map_tstrtstr>>) {
                value = std::pmr::unordered_map<std::pmr::string, std::pmr::string>(mr);
                for(const auto& item : arg) {
                    std::get<std::pmr::unordered_map<std::pmr::string, std::pmr::string>>(value).insert(
                        { CborHelpers::ToPmrString(mr, item.tstrtstr_key), CborHelpers::ToPmrString(mr, item.tstrtstr) }
                    );
                }
            } else {
                throw std::runtime_error("Unsupported property value type");
            }
        },
        prop.value
    );

    return value;
}

UiPropertyType FromCborUiPropertyType(uint32_t type) {
    // Validate before narrowing to the enum's uint16_t underlying type.
    if(type == static_cast<uint32_t>(UiPropertyType::NONE) || type >= static_cast<uint32_t>(UiPropertyType::COUNT))
        throw std::invalid_argument("Invalid UI configuration. Unknown UI property ID: " + std::to_string(type));

    return static_cast<UiPropertyType>(type);
}

void CborPropertyValueTypeToUiValueType(
    std::pmr::memory_resource* mr,
    std::pmr::unordered_map<UiPropertyType, ConfigValue>& properties,
    const CborPropertiesConfig& properties_config
) {

    for(const auto& property : properties_config.CborPropertyValueType_m) {
        auto type = FromCborUiPropertyType(property.CborPropertyValueType_m_key);
        auto [it, inserted] = properties.emplace(type, FromCborPropertyValue(mr, property.CborPropertyValueType_m));
        if(!inserted)
            throw std::invalid_argument(
                "Invalid UI configuration. Duplicate property ID: "
                + std::to_string(property.CborPropertyValueType_m_key)
            );
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
    const CborPropertiesConfig& properties_config
) {

    for(const auto& property : properties_config.CborPropertyValueType_m) {
        auto type = FromCborWidgetPropertyType(property.CborPropertyValueType_m_key);
        auto [it, inserted] = properties.emplace(type, FromCborPropertyValue(mr, property.CborPropertyValueType_m));
        if(!inserted)
            throw std::invalid_argument(
                "Invalid UI configuration. Duplicate property ID: "
                + std::to_string(property.CborPropertyValueType_m_key)
            );
    }
}

void ToCborBindings(
    std::pmr::vector<CborPropertyBinding>& bindings_config,
    const std::pmr::vector<PropertyBinding>& bindings
) {
    for(const auto& binding : bindings) {
        CborPropertyBinding binding_config(std::allocator_arg, Mrm::GetExtPmr());

        binding_config.target = static_cast<uint32_t>(binding.target);
        binding_config.channel = static_cast<uint32_t>(binding.event.channel);
        binding_config.event_type = binding.event.event_type;
        binding_config.payload_key = binding.event.payload_key;
        binding_config.direction = static_cast<uint32_t>(binding.direction);
        binding_config.outbound_event_type = binding.outbound_event_type;
        binding_config.selector_key = binding.event.selector_key;
        binding_config.has_selector = binding.event.HasSelector();

        // The schema has no null, so an absent selector still has to encode some value.
        if(binding_config.has_selector)
            ToCborPropertyValue(binding_config.selector_value, binding.event.selector_value);
        else
            ToCborPropertyValue(binding_config.selector_value, ConfigValue{ 0 });

        bindings_config.push_back(std::move(binding_config));
    }
}

void FromCborBindings(
    std::pmr::memory_resource* mr,
    std::pmr::vector<PropertyBinding>& bindings,
    const std::pmr::vector<CborPropertyBinding>& bindings_config
) {

    for(const auto& binding_config : bindings_config) {
        PropertyBinding binding;

        binding.target = FromCborWidgetPropertyType(binding_config.target);
        binding.event.channel = static_cast<EventChannelId>(binding_config.channel);
        binding.event.event_type = binding_config.event_type;
        binding.event.payload_key = binding_config.payload_key;
        binding.direction = static_cast<PropertyBindingDirection>(binding_config.direction);
        binding.outbound_event_type = binding_config.outbound_event_type;
        binding.event.selector_key = binding_config.selector_key;

        if(binding_config.has_selector)
            binding.event.selector_value = FromCborPropertyValue(mr, binding_config.selector_value);

        bindings.push_back(std::move(binding));
    }
}

template<typename T>
void ValueTypeToCborPropertyValueType(
    CborPropertiesConfig& properties_config,
    const std::pmr::unordered_map<T, ConfigValue>& properties
) {

    for(const auto& [type, value] : properties) {
        CborPropertiesConfig_CborPropertyValueType_m property(std::allocator_arg, Mrm::GetExtPmr());
        property.CborPropertyValueType_m_key = static_cast<uint32_t>(type);
        ToCborPropertyValue(property.CborPropertyValueType_m, value);
        properties_config.CborPropertyValueType_m.push_back(std::move(property));
    }
}

} // namespace

UiConfigurationCborParser::UiConfigurationCborParser(UiConfigurationValidator::ChildValidator validate_children) :
    validate_children_(std::move(validate_children)) {}

PmrUniquePtr<CborUiConfig> UiConfigurationCborParser::Serialize(const UiConfiguration& configuration) {
    UiConfigurationValidator::Validate(configuration, validate_children_);

    auto config = make_unique_pmr<CborUiConfig>(Mrm::GetExtPmr());

    config->version = CONFIGURATION_VERSION;
    config->active_screen_group_id = configuration.active_screen_group_id;

    config->properties_present = !configuration.properties.empty();
    if(!configuration.properties.empty())
        ValueTypeToCborPropertyValueType<UiPropertyType>(config->properties, configuration.properties);

    config->CborScreenConfig_m.clear();
    for(const auto& screen : configuration.screen_configurations) {
        CborScreenConfig screen_config(std::allocator_arg, Mrm::GetExtPmr());

        screen_config.id = screen->id;
        screen_config.group_id = screen->screen_group_id;
        screen_config.type = static_cast<uint32_t>(screen->type);
        screen_config.z_index = screen->z_index;
        screen_config.is_visible = screen->is_visible;
        screen_config.is_overlay = screen->is_overlay;
        screen_config.grid.snap_enabled = screen->grid.snap_enabled;
        screen_config.grid.width = screen->grid.width;
        screen_config.grid.height = screen->grid.height;
        screen_config.grid.spacing_px = screen->grid.spacing_px;

        screen_config.CborWidgetConfig_m.clear();
        for(const auto& widget : screen->widget_configurations) {
            CborWidgetConfig widget_config(std::allocator_arg, Mrm::GetExtPmr());

            widget_config.id = widget->id;
            widget_config.type = static_cast<uint32_t>(widget->type);
            widget_config.position.x = widget->position_grid.x;
            widget_config.position.y = widget->position_grid.y;
            widget_config.size.width = widget->size_grid.width;
            widget_config.size.height = widget->size_grid.height;
            widget_config.z_index = widget->z_index;

            widget_config.properties_present = !widget->properties.empty();
            if(widget_config.properties_present)
                ValueTypeToCborPropertyValueType<WidgetPropertyType>(widget_config.properties, widget->properties);

            widget_config.CborPropertyBinding_m.clear();
            ToCborBindings(widget_config.CborPropertyBinding_m, widget->bindings);

            screen_config.CborWidgetConfig_m.push_back(std::move(widget_config));
        }

        config->CborScreenConfig_m.push_back(std::move(screen_config));
    }

    return config;
}

PmrUniquePtr<UiConfiguration>
UiConfigurationCborParser::Deserialize(std::pmr::memory_resource* mr, const CborUiConfig& config) {

    // Nothing migrates, so a mismatch lets UiConfigurationManager fall through to its default.
    if(config.version != CONFIGURATION_VERSION)
        throw std::invalid_argument(
            "Invalid UI configuration. Unsupported version: " + std::to_string(config.version)
            + ". Expected: " + std::to_string(CONFIGURATION_VERSION) + "."
        );

    auto configuration = make_unique_pmr<UiConfiguration>(mr);

    configuration->active_screen_group_id = config.active_screen_group_id;

    if(config.properties_present)
        CborPropertyValueTypeToUiValueType(mr, configuration->properties, config.properties);

    for(const auto& i : config.CborScreenConfig_m) {
        auto screen_configuration = make_shared_pmr<ScreenConfiguration>(mr);
        screen_configuration->id = i.id;
        screen_configuration->screen_group_id = i.group_id;
        screen_configuration->type = static_cast<ScreenType>(i.type);
        screen_configuration->z_index = i.z_index;
        screen_configuration->is_visible = i.is_visible;
        screen_configuration->is_overlay = i.is_overlay;

        screen_configuration->grid.snap_enabled = i.grid.snap_enabled;
        screen_configuration->grid.width = i.grid.width;
        screen_configuration->grid.height = i.grid.height;
        screen_configuration->grid.spacing_px = i.grid.spacing_px;

        for(const auto& widget : i.CborWidgetConfig_m) {
            auto widget_configuration = make_shared_pmr<WidgetConfiguration>(mr);
            widget_configuration->type = static_cast<WidgetType>(widget.type);
            widget_configuration->id = widget.id;
            widget_configuration->position_grid.x = widget.position.x;
            widget_configuration->position_grid.y = widget.position.y;
            widget_configuration->size_grid.width = widget.size.width;
            widget_configuration->size_grid.height = widget.size.height;
            widget_configuration->z_index = widget.z_index;

            if(widget.properties_present)
                CborPropertyValueTypeToWidgetValueType(mr, widget_configuration->properties, widget.properties);

            FromCborBindings(mr, widget_configuration->bindings, widget.CborPropertyBinding_m);

            screen_configuration->AddWidget(std::move(widget_configuration));
        }

        configuration->screen_configurations.push_back(std::move(screen_configuration));
    }

    UiConfigurationValidator::Validate(*configuration, validate_children_);

    return configuration;
}

} // namespace eerie_leap::domain::ui_domain::configuration::parsers
