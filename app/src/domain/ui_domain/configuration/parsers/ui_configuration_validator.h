#pragma once

#include <functional>
#include <optional>
#include <span>

#include "domain/ui_domain/models/ui_configuration.h"

#include "subsys/event_bus/event_binding.h"

namespace eerie_leap::domain::ui_domain::configuration::parsers {

using eerie_leap::domain::ui_domain::models::ScreenConfiguration;
using eerie_leap::domain::ui_domain::models::UiConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetConfiguration;
using eerie_leap::subsys::event_bus::EventBindingChecker;

class UiConfigurationValidator {
public:
    using ChildValidator = std::function<void(const WidgetConfiguration&, std::span<const WidgetConfiguration* const>)>;

private:
    static void ValidateScreenCount(const UiConfiguration& configuration);
    static void ValidateScreenId(const UiConfiguration& configuration);
    static void ValidateScreenType(const UiConfiguration& configuration);
    static void ValidateScreenGrid(const UiConfiguration& configuration);
    static void ValidateActiveScreenGroupId(const UiConfiguration& configuration);

    static void ValidateScreens(const UiConfiguration& configuration, const ChildValidator& validate_children);

    static void ValidateWidgetType(const ScreenConfiguration& screen_configuration);
    static void
    ValidateWidgetSize(const ScreenConfiguration& screen_configuration, std::span<const std::optional<size_t>> parents);
    static void ValidateWidgetPosition(
        const ScreenConfiguration& screen_configuration,
        std::span<const std::optional<size_t>> parents
    );
    static void ValidateLayoutBindings(
        const ScreenConfiguration& screen_configuration,
        std::span<const std::optional<size_t>> parents
    );
    static void ValidateWidgetProperties(const ScreenConfiguration& screen_configuration);
    static void ValidateWidgetBindings(const ScreenConfiguration& screen_configuration);

public:
    // Widget-owned child rules come from the view layer (the widget factory). Omitting
    // them runs only the domain checks, including the composition graph.
    static void Validate(const UiConfiguration& configuration, const ChildValidator& validate_children = {});

    // Checks every widget binding against the unit applying the configuration: @p check_binding returns
    // why a binding does not resolve there, or an empty text. An inbound binding is checked as written,
    // an outbound one under its outbound event type. Throws std::invalid_argument naming the widget.
    static void ValidateBindings(const UiConfiguration& configuration, const EventBindingChecker& check_binding);

    // Checks the sensor each control drives against the unit applying the configuration: @p check_sensor_input
    // is asked about every outbound binding on the Sensors channel that publishes InputRequested, under that
    // event type, and returns why the sensor it selects is no control sensor of the unit, or an empty text.
    // Throws std::invalid_argument naming the widget.
    static void
    ValidateSensorInputs(const UiConfiguration& configuration, const EventBindingChecker& check_sensor_input);

    // Checks a screen and invokes the supplied widget-owned child rules once.
    static void Validate(const ScreenConfiguration& configuration, const ChildValidator& validate_children = {});
};

} // namespace eerie_leap::domain::ui_domain::configuration::parsers
