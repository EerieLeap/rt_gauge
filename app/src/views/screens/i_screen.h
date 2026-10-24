#pragma once

#include <memory>
#include <span>
#include <vector>

#include "domain/ui_domain/models/screen_configuration.h"
#include "domain/ui_domain/preview/ui_patch.h"
#include "views/i_renderable.h"
#include "views/widgets/i_widget.h"

namespace eerie_leap::views::screens {

using eerie_leap::domain::ui_domain::models::ScreenConfiguration;
using eerie_leap::domain::ui_domain::preview::WidgetPatch;
using eerie_leap::views::widgets::IWidget;

class IScreen : public virtual IRenderable {
public:
    virtual void Configure(std::shared_ptr<ScreenConfiguration> configuration) = 0;
    virtual std::shared_ptr<ScreenConfiguration> GetConfiguration() const = 0;

    // Takes the editor's changes to the root widgets of this configured screen, which are the difference between
    // its configuration and @p configuration, and makes @p configuration the screen's own. Removed roots go
    // first; a replaced root is built before the one it replaces goes; an updated one keeps its objects
    // (IWidget::ApplyEditorConfiguration()); the roots end in the drawing order the configuration gives. A
    // screen that is rendered renders what it builds, and one that is active activates it. May throw when a
    // widget cannot be built, leaving some of the patches applied: the caller shows the screen again.
    virtual void
    ApplyWidgetPatches(std::shared_ptr<ScreenConfiguration> configuration, std::span<const WidgetPatch> patches) = 0;
    virtual std::shared_ptr<std::vector<std::unique_ptr<IWidget>>> GetWidgets() const = 0;

    virtual uint32_t GetId() const = 0;
    virtual uint32_t GetGroupId() const = 0;
    virtual int32_t GetZIndex() const = 0;
    virtual bool IsVisible() const = 0;

    virtual void OnActivated() = 0;
    virtual void OnDeactivated() = 0;

    ~IScreen() override = default;
};

} // namespace eerie_leap::views::screens
