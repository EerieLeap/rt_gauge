#pragma once

#include <memory>
#include <vector>

#include "domain/ui_domain/models/screen_configuration.h"

#include "views/renderable_base.h"
#include "views/screens/i_screen.h"
#include "views/utilitites/frame.h"
#include "views/widgets/i_widget.h"
#include "views/widgets/widget_context.h"

namespace eerie_leap::views::screens {

using eerie_leap::views::widgets::WidgetContext;

class Screen : public RenderableBase, public IScreen {
protected:
    uint32_t id_;
    std::shared_ptr<Frame> parent_;
    WidgetContext context_;

    std::shared_ptr<std::vector<std::unique_ptr<IWidget>>> widgets_;
    std::shared_ptr<ScreenConfiguration> configuration_;

    bool is_activated_ = false;

    void SetVisibility(bool is_visible);
    // Puts the root widgets in the drawing order the configuration gives, in the vector and in LVGL.
    void ApplyRootOrder();

    int DoRender() override;
    int ApplyTheme(const ITheme& theme) override;

public:
    Screen(uint32_t id, const std::shared_ptr<Frame>& parent, WidgetContext context);

    void Configure(std::shared_ptr<ScreenConfiguration> configuration) override;
    std::shared_ptr<ScreenConfiguration> GetConfiguration() const override;

    uint32_t GetId() const override;
    uint32_t GetGroupId() const override;
    int32_t GetZIndex() const override;
    bool IsVisible() const override;

    void OnActivated() override;
    void OnDeactivated() override;

    std::shared_ptr<std::vector<std::unique_ptr<IWidget>>> GetWidgets() const override;

    void ApplyWidgetPatches(
        std::shared_ptr<ScreenConfiguration> configuration,
        std::span<const WidgetPatch> patches
    ) override;
};

} // namespace eerie_leap::views::screens
