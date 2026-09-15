#pragma once

#include <lvgl.h>
#include <memory>

#include "views/renderable_base.h"

#include "i_icon.h"

namespace eerie_leap::views::widgets::basic::icons {

using eerie_leap::views::RenderableBase;

class IconBase : public RenderableBase, public IIcon {
protected:
    std::shared_ptr<Frame> parent_;
    std::shared_ptr<WidgetPropertyStore> properties_;
    bool is_active_;

    std::shared_ptr<AssetsManager> ui_assets_manager_ = nullptr;

public:
    explicit IconBase(std::shared_ptr<Frame> parent);
    virtual ~IconBase() = default;

    // Factory metadata: declarations must be available without creating LVGL objects.
    static void RegisterProperties(WidgetPropertyStore&) {}

    void SetAssetsManager(std::shared_ptr<AssetsManager> ui_assets_manager) override;

    void SetIsActive(bool is_active) override;
    void Configure(std::shared_ptr<WidgetPropertyStore> properties) override;
};

} // namespace eerie_leap::views::widgets::basic::icons
