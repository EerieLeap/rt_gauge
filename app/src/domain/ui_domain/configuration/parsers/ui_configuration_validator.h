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
private:
    static void ValidateScreenCount(const UiConfiguration& configuration);
    static void ValidateScreenId(const UiConfiguration& configuration);
    static void ValidateScreenType(const UiConfiguration& configuration);
    static void ValidateScreenGrid(const UiConfiguration& configuration);
    static void ValidateActiveScreenGroupId(const UiConfiguration& configuration);

    static void ValidateScreens(const UiConfiguration& configuration);

    static void ValidateWidgets(const ScreenConfiguration& screen_configuration);
    static void ValidateWidgetId(const ScreenConfiguration& screen_configuration);
    static void ValidateWidgetType(const ScreenConfiguration& screen_configuration);
    static void ValidateWidgetSize(
        const ScreenConfiguration& screen_configuration,
        std::span<const std::optional<size_t>> parents = {});
    static void ValidateWidgetPosition(
        const ScreenConfiguration& screen_configuration,
        std::span<const std::optional<size_t>> parents = {});
    static void ValidateWidgetProperties(const ScreenConfiguration& screen_configuration);
    static void ValidateWidgetBindings(const ScreenConfiguration& screen_configuration);

public:
    static void Validate(const UiConfiguration& configuration);
    using ChildValidator = std::function<void(const WidgetConfiguration&, std::span<const WidgetConfiguration* const>)>;

    // Checks a screen and invokes the supplied widget-owned child rules once.
    // The domain-only form omits those rules. Production adopts this preflight in
    // Step 8; the existing whole-UI overload retains legacy geometry until then.
    static void Validate(const ScreenConfiguration& configuration, const ChildValidator& validate_children = {});
};

} // namespace eerie_leap::domain::ui_domain::configuration::parsers
