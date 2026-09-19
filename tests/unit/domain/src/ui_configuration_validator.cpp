#include <array>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

#include <zephyr/ztest.h>
#include <eerie_memory.hpp>

#include "utilities/memory/memory_resource_manager.h"

#include "domain/ui_domain/models/ui_configuration.h"
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

    for(int type : { 0, 1, 2 }) {
        properties[WidgetPropertyType::ANIMATION_TYPE] = type;
        for(bool active : { false, true }) {
            properties[WidgetPropertyType::IS_ANIMATION_ACTIVE] = active;
            for(int duration : { 2, 3, 1000, INT32_MAX }) {
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
        for(int value : { INT32_MIN, -1 })
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
