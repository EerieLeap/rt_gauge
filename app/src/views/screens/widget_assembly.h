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

// One screen's widget forest, staged under its own frame. The frame stays hidden
// and processing-disabled until Commit; destruction releases the whole tree.
class WidgetAssembly {
public:
    using Roots = std::vector<std::unique_ptr<IWidget>>;

private:
    std::shared_ptr<Frame> frame_;
    std::shared_ptr<Roots> roots_;

    WidgetAssembly(std::shared_ptr<Frame> frame, std::shared_ptr<Roots> roots);

    void Release();

public:
    // Requires a configuration that passed UiConfigurationValidator::Validate with
    // the factory's child rules and has not changed since. Any failure throws after
    // destroying every staged widget, subscription, and LVGL object.
    static WidgetAssembly Assemble(
        std::shared_ptr<ScreenConfiguration> configuration,
        std::shared_ptr<Frame> container,
        const WidgetContext& context);

    ~WidgetAssembly();

    WidgetAssembly(const WidgetAssembly&) = delete;
    WidgetAssembly& operator=(const WidgetAssembly&) = delete;
    WidgetAssembly(WidgetAssembly&& other) noexcept;
    WidgetAssembly& operator=(WidgetAssembly&& other) noexcept;

    std::shared_ptr<Frame> GetFrame() const;
    // Roots only, in definition order; descendants belong to their owners.
    std::shared_ptr<Roots> GetRoots() const;

    // Reveals the tree and enables processing. Activation remains the screen's job.
    void Commit();
};

} // namespace eerie_leap::views::screens
