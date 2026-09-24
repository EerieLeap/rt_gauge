#pragma once

#include <functional>
#include <optional>
#include <span>

#include "domain/ui_domain/models/ui_configuration.h"

namespace eerie_leap::domain::ui_domain::configuration::parsers {

using eerie_leap::domain::ui_domain::models::UiConfiguration;
using eerie_leap::domain::ui_domain::models::ScreenConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetConfiguration;

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
    static void ValidateWidgetSize(
        const ScreenConfiguration& screen_configuration,
        std::span<const std::optional<size_t>> parents);
    static void ValidateWidgetPosition(
        const ScreenConfiguration& screen_configuration,
        std::span<const std::optional<size_t>> parents);
    static void ValidateWidgetProperties(const ScreenConfiguration& screen_configuration);
    static void ValidateWidgetBindings(const ScreenConfiguration& screen_configuration);

public:
    // Widget-owned child rules come from the view layer (the widget factory). Omitting
    // them runs only the domain checks, including the composition graph.
    static void Validate(const UiConfiguration& configuration, const ChildValidator& validate_children = {});

    // Checks a screen and invokes the supplied widget-owned child rules once.
    static void Validate(const ScreenConfiguration& configuration, const ChildValidator& validate_children = {});
};

} // namespace eerie_leap::domain::ui_domain::configuration::parsers
