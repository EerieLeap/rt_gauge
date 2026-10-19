#pragma once

#include <memory>
#include <vector>

#include "domain/ui_domain/models/screen_configuration.h"
#include "views/i_renderable.h"
#include "views/widgets/i_widget.h"

namespace eerie_leap::views::screens {

using eerie_leap::domain::ui_domain::models::ScreenConfiguration;
using eerie_leap::views::widgets::IWidget;

class IScreen : public virtual IRenderable {
public:
    virtual void Configure(std::shared_ptr<ScreenConfiguration> configuration) = 0;
    virtual std::shared_ptr<ScreenConfiguration> GetConfiguration() const = 0;
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
