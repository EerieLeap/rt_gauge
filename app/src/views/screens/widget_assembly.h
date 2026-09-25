#pragma once

#include <memory>
#include <vector>

#include "domain/ui_domain/models/screen_configuration.h"

#include "views/utilitites/frame.h"
#include "views/widgets/i_widget.h"
#include "views/widgets/widget_context.h"

namespace eerie_leap::views::screens {

using eerie_leap::domain::ui_domain::models::ScreenConfiguration;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::widgets::IWidget;
using eerie_leap::views::widgets::WidgetContext;

// Builds one screen's widget forest directly under the screen's container.
class WidgetAssembly {
public:
    using Roots = std::vector<std::unique_ptr<IWidget>>;

    // Requires a configuration that passed UiConfigurationValidator::Validate with
    // the factory's child rules and has not changed since. Any failure throws after
    // destroying every widget, subscription, and LVGL object it created.
    // Roots keep definition order; descendants belong to their owners.
    static Roots Assemble(
        std::shared_ptr<ScreenConfiguration> configuration,
        std::shared_ptr<Frame> container,
        const WidgetContext& context);
};

} // namespace eerie_leap::views::screens
