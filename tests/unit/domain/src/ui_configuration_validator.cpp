#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"

#include "domain/ui_domain/models/ui_configuration.h"
#include "domain/ui_domain/models/widget_composition.h"
#include "domain/ui_domain/models/widget_type.h"
#include "domain/ui_domain/models/widget_property.h"
#include "domain/ui_domain/models/widget_fill_mode.h"
#include "domain/ui_domain/models/property_binding.h"
#include "domain/ui_domain/configuration/parsers/ui_configuration_validator.h"
#include "domain/ui_domain/utilities/widget_property_validator.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::domain::ui_domain::configuration::parsers;

ZTEST_SUITE(ui_configuration_validator, NULL, NULL, NULL, NULL, NULL);

ZTEST(ui_configuration_validator, test_color_property_mapping_uses_explicit_membership) {
    using namespace eerie_leap::domain::ui_domain::utilities;
    constexpr std::array expected {
        WidgetPropertyType::COLOR_TERTIARY_INACTIVE, WidgetPropertyType::COLOR_PRIMARY_ACTIVE,
        WidgetPropertyType::COLOR_SECONDARY_INACTIVE, WidgetPropertyType::COLOR_TERTIARY_ACTIVE,
        WidgetPropertyType::COLOR_PRIMARY_INACTIVE, WidgetPropertyType::COLOR_SECONDARY_ACTIVE
    };
    zassert_equal(WidgetPropertyValidator::color_properties.size(), expected.size());
    std::array<bool, WidgetPropertyValidator::color_properties.size()> seen{};
    for(auto type : expected) {
        const auto index = WidgetPropertyValidator::GetColorPropertyIndex(type);
        zassert_true(index.has_value());
        zassert_true(*index < seen.size());
        zassert_false(seen[*index]);
        seen[*index] = true;
        zassert_equal(WidgetPropertyValidator::color_properties[*index], type);
        zassert_true(WidgetPropertyValidator::IsColorProperty(type));
    }
    for(std::uint16_t id = 0; id <= static_cast<std::uint16_t>(WidgetPropertyType::COUNT); ++id) {
        const auto type = static_cast<WidgetPropertyType>(id);
        bool is_color = false;
        for(auto color : expected)
            is_color |= type == color;
        zassert_equal(WidgetPropertyValidator::GetColorPropertyIndex(type).has_value(), is_color);
        zassert_equal(WidgetPropertyValidator::IsColorProperty(type), is_color);
    }
    zassert_false(WidgetPropertyValidator::GetColorPropertyIndex(
        static_cast<WidgetPropertyType>(UINT16_MAX)).has_value());
}

namespace {

std::shared_ptr<WidgetConfiguration> MakeWidget(uint32_t id) {
    auto widget_configuration = make_shared_pmr<WidgetConfiguration>(Mrm::GetDefaultPmr());
    widget_configuration->type = WidgetType::IndicatorDigital;
    widget_configuration->id = id;
    widget_configuration->position_grid.x = 0;
    widget_configuration->position_grid.y = 0;
    widget_configuration->size_grid.width = 3;
    widget_configuration->size_grid.height = 3;

    return widget_configuration;
}

std::shared_ptr<ScreenConfiguration> MakeScreen(uint32_t id, uint32_t screen_group_id) {
    auto screen_configuration = make_shared_pmr<ScreenConfiguration>(Mrm::GetDefaultPmr());
    screen_configuration->id = id;
    screen_configuration->screen_group_id = screen_group_id;
    screen_configuration->type = ScreenType::Gauge;
    screen_configuration->grid.snap_enabled = true;
    screen_configuration->grid.width = 3;
    screen_configuration->grid.height = 3;
    screen_configuration->grid.spacing_px = 0;

    screen_configuration->AddWidget(MakeWidget(0));

    return screen_configuration;
}

pmr_unique_ptr<UiConfiguration> MakeConfiguration() {
    auto configuration = make_unique_pmr<UiConfiguration>(Mrm::GetDefaultPmr());
    configuration->active_screen_group_id = 1;
    configuration->screen_configurations.push_back(MakeScreen(0, 1));

    return configuration;
}

bool Validates(const UiConfiguration& configuration) {
    try {
        UiConfigurationValidator::Validate(configuration);
    } catch(const std::invalid_argument&) {
        return false;
    }

    return true;
}

PropertyBinding MakeSensorBinding() {
    PropertyBinding binding;
    binding.target = WidgetPropertyType::VALUE;
    binding.channel = EventChannelId::Sensors;
    binding.event_type = 0;
    binding.payload_key = 1;
    binding.selector_key = 0;
    binding.selector_value = std::pmr::string("sensor_1");

    return binding;
}

void AddBinding(UiConfiguration& configuration, PropertyBinding binding) {
    configuration.screen_configurations[0]->widget_configurations[0]->bindings.push_back(std::move(binding));
}

} // namespace

ZTEST(ui_configuration_validator, test_valid_configuration) {
    zassert_true(Validates(*MakeConfiguration()));
}

ZTEST(ui_configuration_validator, test_only_child_widget_ids_are_structural) {
    using eerie_leap::domain::ui_domain::utilities::WidgetPropertyValidator;
    for(uint16_t id = 0; id <= static_cast<uint16_t>(WidgetPropertyType::COUNT); ++id) {
        const auto type = static_cast<WidgetPropertyType>(id);
        zassert_equal(WidgetPropertyValidator::IsStructuralProperty(type), type == WidgetPropertyType::CHILD_WIDGET_IDS);
    }
    zassert_false(WidgetPropertyValidator::IsStructuralProperty(static_cast<WidgetPropertyType>(UINT16_MAX)));
}

ZTEST(ui_configuration_validator, test_child_widget_ids_require_exact_integer_list_kind) {
    auto configuration = MakeConfiguration();
    auto& screen = *configuration->screen_configurations[0];
    auto& properties = screen.widget_configurations[0]->properties;
    properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::pmr::vector<int>(Mrm::GetDefaultPmr());
    zassert_true(Validates(*configuration));
    for(uint32_t id : { 12U, static_cast<uint32_t>(INT32_MAX) })
        screen.AddWidget(MakeWidget(id));
    properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::pmr::vector<int>({ 12, INT32_MAX }, Mrm::GetDefaultPmr());
    zassert_true(Validates(*configuration));
    const ConfigValue invalid[] = {
        {}, 12, 12.0, true, std::pmr::string("12"),
        std::pmr::vector<std::pmr::string> { std::pmr::string("12") },
        std::pmr::unordered_map<std::pmr::string, std::pmr::string> {}
    };
    for(const auto& value : invalid) {
        properties[WidgetPropertyType::CHILD_WIDGET_IDS] = value;
        zassert_false(Validates(*configuration));
    }
}

ZTEST(ui_configuration_validator, test_structural_bindings_are_rejected_in_every_direction) {
    for(auto direction : { PropertyBindingDirection::In, PropertyBindingDirection::Out, PropertyBindingDirection::InOut }) {
        auto configuration = MakeConfiguration();
        auto binding = MakeSensorBinding();
        binding.target = WidgetPropertyType::CHILD_WIDGET_IDS;
        binding.direction = direction;
        AddBinding(*configuration, binding);
        zassert_false(Validates(*configuration));
    }
}

// Child counts belong to widgets; the domain-only form cannot know that a dial needs a needle.
ZTEST(ui_configuration_validator, test_domain_only_validation_leaves_dial_child_rules_to_widgets) {
    auto configuration = MakeConfiguration();
    auto& widget = *configuration->screen_configurations[0]->widget_configurations[0];
    widget.type = WidgetType::IndicatorDial;
    zassert_true(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_whole_configuration_runs_each_screen_preflight_with_child_rules) {
    auto configuration = MakeConfiguration();
    configuration->screen_configurations.push_back(MakeScreen(1, 1));
    auto& owner = *configuration->screen_configurations[1]->widget_configurations[0];
    auto child = MakeWidget(7);
    child->size_grid = { 9, 9 }; // Child-local geometry is not checked against the screen grid.
    configuration->screen_configurations[1]->AddWidget(child);
    owner.properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::pmr::vector<int>({ 7 }, Mrm::GetDefaultPmr());
    std::vector<std::pair<uint32_t, size_t>> checked;
    UiConfigurationValidator::Validate(*configuration, [&](const auto& widget, auto children) {
        checked.emplace_back(widget.id, children.size());
        if(!children.empty())
            zassert_equal(children[0], child.get());
    });
    const std::vector<std::pair<uint32_t, size_t>> expected { { 0, 0 }, { 0, 1 }, { 7, 0 } };
    zassert_true(checked == expected);

    std::string message;
    try {
        UiConfigurationValidator::Validate(*configuration, [](const auto& widget, auto) {
            if(widget.id == 7)
                throw std::invalid_argument("Widget rule rejected.");
        });
    } catch(const std::invalid_argument& error) {
        message = error.what();
    }
    zassert_true(message.find("Screen ID: 1, Widget ID: 7") != std::string::npos, "%s", message.c_str());
    zassert_true(message.find("Widget rule rejected.") != std::string::npos, "%s", message.c_str());

    owner.properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::pmr::vector<int>({ 8 }, Mrm::GetDefaultPmr());
    zassert_false(Validates(*configuration), "Graph checks run without child rules too");
}

ZTEST(ui_configuration_validator, test_anchor_bindings_remain_ordinary_properties) {
    for(auto target : { WidgetPropertyType::ANCHOR_POINT_X, WidgetPropertyType::ANCHOR_POINT_Y }) {
        for(auto direction : { PropertyBindingDirection::In, PropertyBindingDirection::Out, PropertyBindingDirection::InOut }) {
            auto configuration = MakeConfiguration();
            auto binding = MakeSensorBinding();
            binding.target = target;
            binding.direction = direction;
            AddBinding(*configuration, binding);
            configuration->screen_configurations[0]->widget_configurations[0]->properties[target] = 7;
            zassert_true(Validates(*configuration));
        }
    }
}

ZTEST(ui_configuration_validator, test_anchor_values_are_strict_local_pixel_coordinates) {
    using eerie_leap::domain::ui_domain::utilities::WidgetPropertyValidator;
    auto configuration = MakeConfiguration();
    auto& properties = configuration->screen_configurations[0]->widget_configurations[0]->properties;
    const int valid[] = { -1, 0, 10, 100, 2000, WidgetPropertyValidator::max_anchor_coordinate };
    const ConfigValue invalid[] = {
        {}, true, false, -2, static_cast<int>(INT32_MIN), static_cast<int>(INT32_MAX),
        WidgetPropertyValidator::max_anchor_coordinate + 1,
        -1.0, 10.0, 1.5, std::pmr::string("10"), std::pmr::vector<int> { 10 },
        std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()
    };
    for(auto type : { WidgetPropertyType::ANCHOR_POINT_X, WidgetPropertyType::ANCHOR_POINT_Y }) {
        for(int coordinate : valid) {
            properties[type] = coordinate;
            zassert_true(WidgetPropertyValidator::IsValidAnchorValue(type, properties[type]));
            zassert_true(Validates(*configuration));
        }
        for(const auto& value : invalid) {
            properties[type] = value;
            zassert_false(WidgetPropertyValidator::IsValidAnchorValue(type, value));
            zassert_false(Validates(*configuration));
        }
        properties.erase(type);
    }
    zassert_false(WidgetPropertyValidator::IsValidAnchorValue(WidgetPropertyType::POSITION_X, 10));
}

ZTEST(ui_configuration_validator, test_animation_controls_are_separate_from_management_and_colors) {
    using eerie_leap::domain::ui_domain::utilities::WidgetPropertyValidator;
    for(uint16_t id = 0; id <= static_cast<uint16_t>(WidgetPropertyType::COUNT); ++id) {
        const auto type = static_cast<WidgetPropertyType>(id);
        const bool is_animation = id >= 41 && id <= 43;
        zassert_equal(WidgetPropertyValidator::IsAnimationProperty(type), is_animation);
        if(is_animation) {
            zassert_false(WidgetPropertyValidator::IsManagementProperty(type));
            zassert_false(WidgetPropertyValidator::IsColorProperty(type));
            zassert_false(WidgetPropertyValidator::IsAppearanceProperty(type));
        } else {
            zassert_false(WidgetPropertyValidator::IsValidAnimationValue(type, 2));
            zassert_false(WidgetPropertyValidator::IsValidAnimationValue(type, true));
        }
    }
    zassert_false(WidgetPropertyValidator::IsAnimationProperty(static_cast<WidgetPropertyType>(UINT16_MAX)));
}

ZTEST(ui_configuration_validator, test_animation_defaults_effects_and_duration_boundaries_are_valid) {
    auto configuration = MakeConfiguration();
    auto& properties = configuration->screen_configurations[0]->widget_configurations[0]->properties;
    properties[WidgetPropertyType::ANIMATION_TYPE] = static_cast<int>(Animation::DEFAULT_TYPE);
    properties[WidgetPropertyType::IS_ANIMATION_ACTIVE] = Animation::DEFAULT_ACTIVE;
    properties[WidgetPropertyType::ANIMATION_DURATION_MS] = Animation::DEFAULT_DURATION_MS;
    zassert_true(Validates(*configuration));

    const int durations[] = { 2, 3, 1000, INT32_MAX };
    for(int type : { 0, 1, 2 }) {
        properties[WidgetPropertyType::ANIMATION_TYPE] = type;
        for(bool active : { false, true }) {
            properties[WidgetPropertyType::IS_ANIMATION_ACTIVE] = active;
            for(int duration : durations) {
                properties[WidgetPropertyType::ANIMATION_DURATION_MS] = duration;
                zassert_true(Validates(*configuration));
            }
        }
    }
}

ZTEST(ui_configuration_validator, test_invalid_animation_values_are_rejected_even_when_dormant) {
    using eerie_leap::domain::ui_domain::utilities::WidgetPropertyValidator;
    const ConfigValue invalid_numbers[] = {
        {}, true, false, 0.0, 1.0, 2.0, 2.5, 1000.0,
        static_cast<double>(INT32_MAX) + 1.0,
        std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), std::pmr::string("2")
    };
    auto configuration = MakeConfiguration();
    auto& properties = configuration->screen_configurations[0]->widget_configurations[0]->properties;
    const int invalid_integers[] = { INT32_MIN, -1 };
    for(auto type : { WidgetPropertyType::ANIMATION_TYPE, WidgetPropertyType::ANIMATION_DURATION_MS }) {
        auto reject = [&](const ConfigValue& value) {
            properties[WidgetPropertyType::ANIMATION_TYPE] = 0;
            properties[WidgetPropertyType::IS_ANIMATION_ACTIVE] = false;
            properties[WidgetPropertyType::ANIMATION_DURATION_MS] = 1000;
            properties[type] = value;
            zassert_false(WidgetPropertyValidator::IsValidAnimationValue(type, value));
            zassert_false(Validates(*configuration));
        };
        for(const auto& value : invalid_numbers)
            reject(value);
        for(int value : invalid_integers)
            reject(value);
        if(type == WidgetPropertyType::ANIMATION_TYPE) {
            reject(3);
            reject(INT32_MAX);
        } else {
            reject(0);
            reject(1);
        }
    }

    properties[WidgetPropertyType::ANIMATION_TYPE] = 0;
    properties[WidgetPropertyType::ANIMATION_DURATION_MS] = 1000;
    const ConfigValue invalid_active[] = {
        {}, 0, 1, -1, 2, 0.0, 1.0, 0.5, std::pmr::string("true"),
        std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity()
    };
    for(const auto& value : invalid_active) {
        properties[WidgetPropertyType::IS_ANIMATION_ACTIVE] = value;
        zassert_false(WidgetPropertyValidator::IsValidAnimationValue(WidgetPropertyType::IS_ANIMATION_ACTIVE, value));
        zassert_false(Validates(*configuration));
    }
}

ZTEST(ui_configuration_validator, test_appearance_classification_is_separate_from_value_validation) {
    using namespace eerie_leap::domain::ui_domain::utilities;
    for(auto type : { WidgetPropertyType::IS_ACTIVE, WidgetPropertyType::IS_VISIBLE }) {
        zassert_true(WidgetPropertyValidator::IsAppearanceProperty(type));
        zassert_true(WidgetPropertyValidator::IsValidAppearanceValue(type, true));
        zassert_true(WidgetPropertyValidator::IsValidAppearanceValue(type, false));
        zassert_false(WidgetPropertyValidator::IsValidAppearanceValue(type, 1));
    }
    zassert_true(WidgetPropertyValidator::IsAppearanceProperty(WidgetPropertyType::OPACITY));
    zassert_true(WidgetPropertyValidator::IsValidAppearanceValue(WidgetPropertyType::OPACITY, 128));
    zassert_false(WidgetPropertyValidator::IsValidAppearanceValue(WidgetPropertyType::OPACITY, 256));
    for(auto type : { WidgetPropertyType::COLOR_PRIMARY_ACTIVE, WidgetPropertyType::COLOR_PRIMARY_INACTIVE,
                     WidgetPropertyType::COLOR_SECONDARY_ACTIVE, WidgetPropertyType::COLOR_SECONDARY_INACTIVE,
                     WidgetPropertyType::COLOR_TERTIARY_ACTIVE, WidgetPropertyType::COLOR_TERTIARY_INACTIVE }) {
        zassert_true(WidgetPropertyValidator::IsAppearanceProperty(type));
        zassert_true(WidgetPropertyValidator::IsValidAppearanceValue(type, std::pmr::string("#12345680")));
        zassert_true(WidgetPropertyValidator::IsValidAppearanceValue(type, std::pmr::string("")));
        zassert_false(WidgetPropertyValidator::IsValidAppearanceValue(type, std::pmr::string("#123456")));
    }
    for(auto type : { WidgetPropertyType::VALUE, WidgetPropertyType::LABEL, WidgetPropertyType::IS_SMOOTHED,
                     WidgetPropertyType::DIRECTION, WidgetPropertyType::NONE, WidgetPropertyType::COUNT }) {
        zassert_false(WidgetPropertyValidator::IsAppearanceProperty(type));
        zassert_false(WidgetPropertyValidator::IsValidAppearanceValue(type, 1));
    }
}

ZTEST(ui_configuration_validator, test_color_properties_require_rgba_text_or_empty_reset) {
    auto configuration = MakeConfiguration();
    auto& properties = configuration->screen_configurations[0]->widget_configurations[0]->properties;
    const ConfigValue invalid_values[] = {
        {}, 0, 255, 128.0, true, false, std::pmr::string("#3366FF"), std::pmr::string("3366FF80"),
        std::pmr::string("#3366FF8G"), std::pmr::string("#3366FF800"), std::pmr::string("#3366FF80\0", 10)
    };
    for(auto type : { WidgetPropertyType::COLOR_PRIMARY_ACTIVE, WidgetPropertyType::COLOR_PRIMARY_INACTIVE,
            WidgetPropertyType::COLOR_SECONDARY_ACTIVE, WidgetPropertyType::COLOR_SECONDARY_INACTIVE,
            WidgetPropertyType::COLOR_TERTIARY_ACTIVE, WidgetPropertyType::COLOR_TERTIARY_INACTIVE }) {
        for(auto text : { "", "#00000000", "#FFFFFFFF", "#3366fF80" }) {
            properties[type] = std::pmr::string(text, Mrm::GetExtPmr());
            zassert_true(Validates(*configuration));
        }
        for(const auto& value : invalid_values) {
            properties[type] = value;
            zassert_false(Validates(*configuration));
        }
        properties.erase(type);
        zassert_true(Validates(*configuration));
    }
}

ZTEST(ui_configuration_validator, test_opacity_requires_an_integer_in_byte_range) {
    auto configuration = MakeConfiguration();
    auto& properties = configuration->screen_configurations[0]->widget_configurations[0]->properties;
    for(int opacity : { 0, 128, 255 }) {
        properties[WidgetPropertyType::OPACITY] = opacity;
        zassert_true(Validates(*configuration));
    }
    const ConfigValue invalid_values[] = {
        {}, -1, 256, INT32_MIN, INT32_MAX, 0.0, 128.5, 255.0, true, false, std::pmr::string("128")
    };
    for(const auto& value : invalid_values) {
        properties[WidgetPropertyType::OPACITY] = value;
        zassert_false(Validates(*configuration));
    }
    properties.erase(WidgetPropertyType::OPACITY);
    zassert_true(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_fill_mode_accepts_only_known_numeric_modes) {
    auto configuration = MakeConfiguration();
    auto& properties = configuration->screen_configurations[0]->widget_configurations[0]->properties;
    for(auto mode : { WidgetFillMode::Filled, WidgetFillMode::Outline }) {
        properties[WidgetPropertyType::FILL_MODE] = static_cast<int>(mode);
        zassert_true(Validates(*configuration));
    }
    for(auto value : { -1.0, 2.0, 0.5 }) {
        properties[WidgetPropertyType::FILL_MODE] = value;
        zassert_false(Validates(*configuration));
    }
    properties[WidgetPropertyType::FILL_MODE] = true;
    zassert_false(Validates(*configuration));
    properties[WidgetPropertyType::FILL_MODE] = "Outline";
    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_empty_configuration_is_valid) {
    // The default configuration created on first boot has no screens.
    auto configuration = make_unique_pmr<UiConfiguration>(Mrm::GetDefaultPmr());

    zassert_true(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_the_screen_count_limit_matches_the_schema) {
    auto configuration = MakeConfiguration();

    // MakeConfiguration() already holds one, so this fills the schema's 24 slots.
    for(uint32_t i = 1; i < 24; i++)
        configuration->screen_configurations.push_back(MakeScreen(i, 1));

    zassert_true(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_too_many_screens_is_invalid) {
    auto configuration = MakeConfiguration();

    for(uint32_t i = 1; i <= 24; i++)
        configuration->screen_configurations.push_back(MakeScreen(i, 1));

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_duplicate_screen_id_is_invalid) {
    auto configuration = MakeConfiguration();
    configuration->screen_configurations.push_back(MakeScreen(0, 1));

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_unknown_screen_type_is_invalid) {
    auto configuration = MakeConfiguration();
    configuration->screen_configurations[0]->type = ScreenType::None;

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_empty_grid_is_invalid) {
    auto configuration = MakeConfiguration();
    configuration->screen_configurations[0]->grid.width = 0;

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_active_screen_group_without_screen_is_invalid) {
    auto configuration = MakeConfiguration();
    configuration->active_screen_group_id = 7;

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_duplicate_widget_id_is_invalid) {
    auto configuration = MakeConfiguration();
    configuration->screen_configurations[0]->AddWidget(MakeWidget(0));

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_unknown_widget_type_is_invalid) {
    auto configuration = MakeConfiguration();
    configuration->screen_configurations[0]->widget_configurations[0]->type = WidgetType::None;

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_empty_widget_size_is_invalid) {
    auto configuration = MakeConfiguration();
    configuration->screen_configurations[0]->widget_configurations[0]->size_grid.height = 0;

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_widget_larger_than_grid_is_invalid) {
    auto configuration = MakeConfiguration();
    configuration->screen_configurations[0]->widget_configurations[0]->size_grid.width = 4;

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_negative_widget_position_is_invalid) {
    auto configuration = MakeConfiguration();
    configuration->screen_configurations[0]->widget_configurations[0]->position_grid.y = -1;

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_widget_position_outside_grid_is_invalid) {
    auto configuration = MakeConfiguration();
    configuration->screen_configurations[0]->widget_configurations[0]->position_grid.x = 3;

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_known_widget_properties_are_valid) {
    auto configuration = MakeConfiguration();
    auto& properties = configuration->screen_configurations[0]->widget_configurations[0]->properties;

    properties[WidgetPropertyType::LABEL] = "sensor_1";
    properties[WidgetPropertyType::IS_SMOOTHED] = true;
    properties[WidgetPropertyType::MIN_VALUE] = 0;
    properties[WidgetPropertyType::MAX_VALUE] = 100.5;

    zassert_true(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_unknown_widget_property_is_invalid) {
    for(auto type : { WidgetPropertyType::NONE, WidgetPropertyType::COUNT,
                      static_cast<WidgetPropertyType>(9999), static_cast<WidgetPropertyType>(UINT16_MAX) }) {
        auto configuration = MakeConfiguration();
        configuration->screen_configurations[0]->widget_configurations[0]->properties[type] = 1;

        zassert_false(Validates(*configuration), "Accepted invalid property ID %u.", static_cast<unsigned>(type));
    }
}

ZTEST(ui_configuration_validator, test_widget_property_with_wrong_value_type_is_invalid) {
    auto configuration = MakeConfiguration();
    auto& properties = configuration->screen_configurations[0]->widget_configurations[0]->properties;

    properties[WidgetPropertyType::MIN_VALUE] = true;

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_widget_property_without_value_is_invalid) {
    auto configuration = MakeConfiguration();
    auto& properties = configuration->screen_configurations[0]->widget_configurations[0]->properties;

    properties[WidgetPropertyType::LABEL] = ConfigValue{};

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_settings_screen_type_is_valid) {
    auto configuration = MakeConfiguration();
    configuration->screen_configurations[0]->type = ScreenType::Settings;

    zassert_true(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_popup_screen_type_is_valid) {
    auto configuration = MakeConfiguration();
    configuration->screen_configurations[0]->type = ScreenType::Popup;

    zassert_true(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_control_widget_types_are_valid) {
    constexpr std::array control_types = {
        WidgetType::ControlSlider,
        WidgetType::ControlToggle,
        WidgetType::ControlButton,
        WidgetType::IndicatorSetting
    };

    for(auto control_type : control_types) {
        auto configuration = MakeConfiguration();
        configuration->screen_configurations[0]->widget_configurations[0]->type = control_type;

        zassert_true(Validates(*configuration), "Expected widget type %u to be accepted.", static_cast<uint32_t>(control_type));
    }
}

ZTEST(ui_configuration_validator, test_control_widget_properties_are_valid) {
    auto configuration = MakeConfiguration();
    auto& widget_configuration = configuration->screen_configurations[0]->widget_configurations[0];
    widget_configuration->type = WidgetType::ControlSlider;

    auto& properties = widget_configuration->properties;
    properties[WidgetPropertyType::SETTING_ID] = "display.brightness";
    properties[WidgetPropertyType::UNIT] = "%";
    properties[WidgetPropertyType::STEP] = 5.0;
    properties[WidgetPropertyType::TARGET_SCREEN_GROUP] = 2;

    zassert_true(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_numeric_setting_id_is_invalid) {
    auto configuration = MakeConfiguration();
    auto& properties = configuration->screen_configurations[0]->widget_configurations[0]->properties;

    properties[WidgetPropertyType::SETTING_ID] = 1;

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_a_widget_without_bindings_is_valid) {
    auto configuration = MakeConfiguration();

    zassert_true(configuration->screen_configurations[0]->widget_configurations[0]->bindings.empty());
    zassert_true(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_a_sensor_binding_is_valid) {
    auto configuration = MakeConfiguration();

    AddBinding(*configuration, MakeSensorBinding());

    zassert_true(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_an_unconditional_binding_is_valid) {
    auto configuration = MakeConfiguration();

    auto binding = MakeSensorBinding();
    binding.selector_key = 0;
    binding.selector_value = { };

    zassert_false(binding.HasSelector());

    AddBinding(*configuration, std::move(binding));

    zassert_true(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_several_bindings_may_share_one_source) {
    auto configuration = MakeConfiguration();

    auto first = MakeSensorBinding();
    auto second = MakeSensorBinding();
    second.target = WidgetPropertyType::IS_VISIBLE;

    AddBinding(*configuration, std::move(first));
    AddBinding(*configuration, std::move(second));

    zassert_true(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_a_binding_without_a_channel_is_invalid) {
    auto configuration = MakeConfiguration();

    auto binding = MakeSensorBinding();
    binding.channel = EventChannelId::None;

    AddBinding(*configuration, std::move(binding));

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_an_unknown_channel_is_invalid) {
    auto configuration = MakeConfiguration();

    auto binding = MakeSensorBinding();
    binding.channel = static_cast<EventChannelId>(99);

    AddBinding(*configuration, std::move(binding));

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_an_unknown_direction_is_invalid) {
    auto configuration = MakeConfiguration();

    auto binding = MakeSensorBinding();
    binding.direction = static_cast<PropertyBindingDirection>(99);

    AddBinding(*configuration, std::move(binding));

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_a_binding_without_a_target_is_invalid) {
    auto configuration = MakeConfiguration();

    auto binding = MakeSensorBinding();
    binding.target = WidgetPropertyType::NONE;

    AddBinding(*configuration, std::move(binding));

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_an_unknown_target_property_is_invalid) {
    auto configuration = MakeConfiguration();

    auto binding = MakeSensorBinding();
    binding.target = static_cast<WidgetPropertyType>(9999);

    AddBinding(*configuration, std::move(binding));

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_an_integer_selector_is_valid) {
    auto configuration = MakeConfiguration();

    auto binding = MakeSensorBinding();
    binding.selector_value = 42;

    AddBinding(*configuration, std::move(binding));

    zassert_true(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_a_selector_that_cannot_be_compared_is_invalid) {
    auto configuration = MakeConfiguration();

    // A selector is matched against a uint32 in the payload; a bool cannot reduce to one.
    auto binding = MakeSensorBinding();
    binding.selector_value = true;

    AddBinding(*configuration, std::move(binding));

    zassert_false(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_empty_ui_properties_are_valid) {
    auto configuration = MakeConfiguration();
    zassert_true(configuration->properties.empty());
    zassert_true(Validates(*configuration));
}

ZTEST(ui_configuration_validator, test_undefined_ui_properties_are_invalid) {
    for(auto type : { UiPropertyType::NONE, UiPropertyType::COUNT, static_cast<UiPropertyType>(29),
                      static_cast<UiPropertyType>(9999), static_cast<UiPropertyType>(UINT16_MAX) }) {
        auto configuration = MakeConfiguration();
        configuration->properties[type] = 1;

        zassert_false(Validates(*configuration), "Accepted undefined UI property ID %u.", static_cast<unsigned>(type));
    }
}

namespace {

std::shared_ptr<ScreenConfiguration> MakeCompositionScreen(std::initializer_list<uint32_t> ids) {
    auto screen = MakeScreen(42, 1);
    screen->widget_configurations.clear();
    for(auto id : ids)
        screen->widget_configurations.push_back(MakeWidget(id));
    return screen;
}

void SetChildren(WidgetConfiguration& widget, std::initializer_list<int> ids) {
    widget.properties[WidgetPropertyType::CHILD_WIDGET_IDS] =
        std::pmr::vector<int>(ids, widget.properties.get_allocator().resource());
}

void ExpectCompositionError(const ScreenConfiguration& screen, std::initializer_list<const char*> fragments) {
    std::string message;
    try {
        UiConfigurationValidator::Validate(screen);
    } catch(const std::invalid_argument& error) {
        message = error.what();
    }
    zassert_false(message.empty(), "Expected composition rejection");
    zassert_true(message.find("Screen ID: 42") != std::string::npos, "%s", message.c_str());
    for(const auto* fragment : fragments)
        zassert_true(message.find(fragment) != std::string::npos, "Missing '%s' in '%s'", fragment, message.c_str());
}

} // namespace

ZTEST(ui_configuration_validator, test_composition_empty_screen_and_empty_child_list) {
    auto screen = MakeCompositionScreen({});
    UiConfigurationValidator::Validate(*screen);
    auto empty = WidgetComposition::Build(*screen);
    zassert_true(empty.nodes.empty());
    zassert_true(empty.children.empty());
    zassert_true(empty.roots.empty());
    zassert_true(empty.postorder.empty());
    screen->AddWidget(MakeWidget(0));
    SetChildren(*screen->widget_configurations[0], {});
    UiConfigurationValidator::Validate(*screen);
    auto single = WidgetComposition::Build(*screen);
    zassert_equal(single.roots.size(), 1);
    zassert_equal(single.roots[0], 0);
    zassert_equal(single.postorder[0], 0);
    zassert_false(single.nodes[0].parent.has_value());
}

ZTEST(ui_configuration_validator, test_composition_forward_references_preserve_order_and_id_boundaries) {
    auto screen = MakeCompositionScreen({ 12, 7, 9, 0, UINT32_MAX, INT32_MAX });
    SetChildren(*screen->widget_configurations[2], { INT32_MAX, 7, 0 });
    SetChildren(*screen->widget_configurations[1], { 12 });
    // Neither IDs nor z-order define the owner's semantic child order.
    screen->widget_configurations[5]->z_index = 100;
    screen->widget_configurations[1]->z_index = -100;
    UiConfigurationValidator::Validate(*screen);
    auto graph = WidgetComposition::Build(*screen);
    zassert_equal(graph.roots.size(), 2);
    zassert_equal(graph.roots[0], 2);
    zassert_equal(graph.roots[1], 4);
    const size_t expected_children[] = { 5, 1, 3 };
    const size_t expected_postorder[] = { 5, 0, 1, 3, 2, 4 };
    zassert_equal(graph.GetChildren(2).size(), std::size(expected_children));
    for(size_t i = 0; i < std::size(expected_children); ++i)
        zassert_equal(graph.GetChildren(2)[i], expected_children[i]);
    zassert_equal(graph.postorder.size(), std::size(expected_postorder));
    for(size_t i = 0; i < std::size(expected_postorder); ++i)
        zassert_equal(graph.postorder[i], expected_postorder[i]);
    zassert_equal(*graph.nodes[0].parent, 1);
    zassert_equal(*graph.nodes[1].parent, 2);
}

ZTEST(ui_configuration_validator, test_composition_rejects_duplicate_definitions) {
    auto screen = MakeCompositionScreen({ 0, 0 });
    ExpectCompositionError(*screen, { "Widget ID: 0", "duplicate widget IDs" });
}

ZTEST(ui_configuration_validator, test_composition_rejects_signed_and_missing_references) {
    auto screen = MakeCompositionScreen({ 9, UINT32_MAX });
    for(int id : { -1, INT32_MIN }) {
        SetChildren(*screen->widget_configurations[0], { id });
        ExpectCompositionError(*screen, { "Widget ID: 9", "Child index 0", "between 0 and INT32_MAX" });
    }
    SetChildren(*screen->widget_configurations[0], { 12 });
    // A definition in another screen cannot satisfy the reference.
    auto other_screen = MakeCompositionScreen({ 12 });
    UiConfigurationValidator::Validate(*other_screen);
    ExpectCompositionError(*screen, { "Widget ID: 9", "Child index 0, ID 12", "this screen" });
}

ZTEST(ui_configuration_validator, test_composition_rejects_wrong_reference_kinds_without_coercion) {
    auto screen = MakeCompositionScreen({ 9 });
    for(const ConfigValue& value : { ConfigValue { 9 }, ConfigValue { 9.0 }, ConfigValue { true },
        ConfigValue { std::pmr::string("9") }, ConfigValue {} }) {
        screen->widget_configurations[0]->properties[WidgetPropertyType::CHILD_WIDGET_IDS] = value;
        ExpectCompositionError(*screen, { "Widget ID: 9", "integer list" });
    }
}

ZTEST(ui_configuration_validator, test_composition_rejects_self_duplicate_and_shared_children) {
    auto screen = MakeCompositionScreen({ 9, 12, 3 });
    auto& first = *screen->widget_configurations[0];
    SetChildren(first, { 9 });
    ExpectCompositionError(*screen, { "Widget ID: 9", "Child index 0, ID 9", "self-reference" });
    SetChildren(first, { 12, 12 });
    ExpectCompositionError(*screen, { "Widget ID: 9", "Child index 1, ID 12", "duplicate reference" });
    SetChildren(first, { 12 });
    SetChildren(*screen->widget_configurations[2], { 12 });
    ExpectCompositionError(*screen, { "Widget ID: 3", "Child index 0, ID 12", "already owned by widget 9" });
}

ZTEST(ui_configuration_validator, test_composition_finds_cycles_with_and_without_unrelated_roots) {
    for(bool with_root : { false, true }) {
        auto screen = MakeCompositionScreen({ 9, 12, 3 });
        SetChildren(*screen->widget_configurations[0], { 12 });
        SetChildren(*screen->widget_configurations[1], { 3 });
        SetChildren(*screen->widget_configurations[2], { 9 });
        if(with_root)
            screen->widget_configurations.insert(screen->widget_configurations.begin(), MakeWidget(99));
        ExpectCompositionError(*screen, { "Widget ID: 3", "Child index 0, ID 9", "cycle 9 -> 12 -> 3 -> 9" });
    }
}

ZTEST(ui_configuration_validator, test_composition_accepts_exact_node_and_edge_limits) {
    auto screen = MakeCompositionScreen({ 0 });
    std::pmr::vector<int> children(Mrm::GetDefaultPmr());
    for(size_t i = 1; i < WidgetComposition::max_nodes; ++i) {
        screen->widget_configurations.push_back(MakeWidget(i));
        children.push_back(i);
    }
    screen->widget_configurations[0]->properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::move(children);
    UiConfigurationValidator::Validate(*screen);
    auto graph = WidgetComposition::Build(*screen);
    zassert_equal(graph.nodes.size(), 32);
    zassert_equal(graph.children.size(), 31);
    zassert_equal(graph.GetChildren(0).size(), 31);
    zassert_equal(graph.roots.size(), 1);
    zassert_equal(graph.postorder.back(), 0);
    // Even an invalid 32nd edge is rejected by the resource limit before graph traversal.
    std::get<std::pmr::vector<int>>(screen->widget_configurations[0]->properties[WidgetPropertyType::CHILD_WIDGET_IDS])
        .push_back(1);
    ExpectCompositionError(*screen, { "Widget ID: 0", "31 edges" });
    screen->AddWidget(MakeWidget(32));
    ExpectCompositionError(*screen, { "32 nodes" });
}

ZTEST(ui_configuration_validator, test_composition_depth_counts_root_and_is_independent_of_definition_order) {
    for(bool reversed : { false, true }) {
        auto screen = MakeCompositionScreen({});
        for(size_t i = 0; i < WidgetComposition::max_depth; ++i) {
            auto widget = MakeWidget(i);
            if(i + 1 < WidgetComposition::max_depth)
                SetChildren(*widget, { static_cast<int>(i + 1) });
            screen->widget_configurations.push_back(widget);
        }
        if(reversed)
            std::reverse(screen->widget_configurations.begin(), screen->widget_configurations.end());
        UiConfigurationValidator::Validate(*screen);
        auto graph = WidgetComposition::Build(*screen);
        zassert_equal(graph.postorder.size(), 8);
        auto root = MakeWidget(99);
        SetChildren(*root, { 0 });
        screen->widget_configurations.push_back(root);
        ExpectCompositionError(*screen, { "Widget ID: 99", "8 levels", "root is level 1" });
    }
}

ZTEST(ui_configuration_validator, test_composition_separates_root_grid_and_child_local_geometry) {
    auto screen = MakeCompositionScreen({ 9, 12 });
    SetChildren(*screen->widget_configurations[0], { 12 });
    auto& child = *screen->widget_configurations[1];
    child.position_grid = { -100, 500 };
    child.size_grid = { 0, 999 };
    child.properties[WidgetPropertyType::ANCHOR_POINT_X] = 2000;
    UiConfigurationValidator::Validate(*screen); // The owner's view contract decides local layout.
    child.properties[WidgetPropertyType::ANCHOR_POINT_X] = -2;
    ExpectCompositionError(*screen, { "Widget ID: 12", "anchor coordinate" });
    child.properties.clear();
    auto& root = *screen->widget_configurations[0];
    root.size_grid = { 0, 1 };
    ExpectCompositionError(*screen, { "Widget ID: 9", "size must be greater than 0" });
    root.size_grid = { 4, 1 };
    ExpectCompositionError(*screen, { "Widget ID: 9", "width cannot exceed" });
    root.size_grid = { 1, 4 };
    ExpectCompositionError(*screen, { "Widget ID: 9", "height cannot exceed" });
    root.size_grid = { 1, 1 };
    root.position_grid = { -1, 0 };
    ExpectCompositionError(*screen, { "Widget ID: 9", "position cannot be negative" });
    root.position_grid = { 3, 0 };
    ExpectCompositionError(*screen, { "Widget ID: 9", "position X" });
    root.position_grid = { 0, 3 };
    ExpectCompositionError(*screen, { "Widget ID: 9", "position Y" });
}

ZTEST(ui_configuration_validator, test_composition_checks_child_types_properties_and_bindings) {
    auto screen = MakeCompositionScreen({ 9, 12 });
    SetChildren(*screen->widget_configurations[0], { 12 });
    auto& child = *screen->widget_configurations[1];
    child.type = static_cast<WidgetType>(255);
    ExpectCompositionError(*screen, { "Widget ID: 12", "Invalid widget type" });
    child.type = WidgetType::IndicatorDial; // Domain graph has no widget-specific count rule.
    UiConfigurationValidator::Validate(*screen);
    for(auto direction : { PropertyBindingDirection::In, PropertyBindingDirection::Out, PropertyBindingDirection::InOut }) {
        auto binding = MakeSensorBinding();
        binding.target = WidgetPropertyType::CHILD_WIDGET_IDS;
        binding.direction = direction;
        child.bindings = { binding };
        ExpectCompositionError(*screen, { "Widget ID: 12", "structural property" });
    }
}

ZTEST(ui_configuration_validator, test_composition_propagates_screen_allocator) {
    std::array<std::byte, 2048> storage;
    std::pmr::monotonic_buffer_resource resource(storage.data(), storage.size(), std::pmr::null_memory_resource());
    auto screen = make_shared_pmr<ScreenConfiguration>(&resource);
    screen->id = 42;
    screen->grid = { .width = 3, .height = 3 };
    screen->widget_configurations.push_back(MakeWidget(9));
    screen->widget_configurations.push_back(MakeWidget(12));
    SetChildren(*screen->widget_configurations[0], { 12 });
    UiConfigurationValidator::Validate(*screen);
    auto graph = WidgetComposition::Build(*screen);
    auto moved = std::move(graph);
    zassert_equal(moved.nodes.get_allocator().resource(), &resource);
    zassert_equal(moved.children.get_allocator().resource(), &resource);
    zassert_equal(moved.roots.get_allocator().resource(), &resource);
    zassert_equal(moved.postorder.get_allocator().resource(), &resource);
    zassert_equal(moved.GetChildren(0)[0], 1);
}

ZTEST(ui_configuration_validator, test_composition_rejects_null_definitions_and_zero_grid) {
    auto screen = MakeCompositionScreen({ 9 });
    screen->widget_configurations.push_back(nullptr);
    ExpectCompositionError(*screen, { "null widget definition" });
    screen->widget_configurations.pop_back();
    screen->grid.width = 0;
    ExpectCompositionError(*screen, { "Grid dimensions" });
}

ZTEST(ui_configuration_validator, test_composition_size_limits_are_checked_before_graph_allocation) {
    class AllocationGate : public std::pmr::memory_resource {
        void* do_allocate(size_t bytes, size_t alignment) override {
            if(reject_allocations)
                throw std::bad_alloc();
            return std::pmr::new_delete_resource()->allocate(bytes, alignment);
        }
        void do_deallocate(void* pointer, size_t bytes, size_t alignment) override {
            std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
        }
        bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
            return this == &other;
        }
    public:
        bool reject_allocations = false;
    } resource;

    auto screen = make_shared_pmr<ScreenConfiguration>(&resource);
    screen->id = 42;
    screen->grid = { .width = 3, .height = 3 };
    for(size_t i = 0; i < 33; ++i)
        screen->widget_configurations.push_back(MakeWidget(i));
    resource.reject_allocations = true;
    ExpectCompositionError(*screen, { "32 nodes" });
    screen->widget_configurations.pop_back();
    screen->widget_configurations[0]->properties[WidgetPropertyType::CHILD_WIDGET_IDS] =
        std::pmr::vector<int>(32, 1, Mrm::GetDefaultPmr());
    ExpectCompositionError(*screen, { "Widget ID: 0", "Child index 31, ID 1", "31 edges" });
}
