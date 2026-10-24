#pragma once

#include <cstdint>
#include <memory>
#include <optional>
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
        const std::shared_ptr<ScreenConfiguration>& configuration,
        const std::shared_ptr<Frame>& container,
        const WidgetContext& context
    );

    // One root widget with the subtree it owns, built under @p container like Assemble() builds it and
    // placed on the screen's grid; the rest of the screen is left alone. Appended to the container's
    // children: the caller puts it in its place. Requires a root's ID.
    static std::unique_ptr<IWidget> AssembleRoot(
        const std::shared_ptr<ScreenConfiguration>& configuration,
        uint32_t root_id,
        const std::shared_ptr<Frame>& container,
        const WidgetContext& context
    );

private:
    static Roots AssembleRoots(
        const std::shared_ptr<ScreenConfiguration>& configuration,
        std::optional<uint32_t> only_root_id,
        const std::shared_ptr<Frame>& container,
        const WidgetContext& context
    );
};

} // namespace eerie_leap::views::screens
