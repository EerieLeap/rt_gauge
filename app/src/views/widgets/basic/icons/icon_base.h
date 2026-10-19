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
    bool is_processing_enabled_ = false;

    bool IsProcessingEligible() const;

    // The unit's asset registry, in which an icon looks its files up by kind.
    std::shared_ptr<AssetsManager> ui_assets_manager_ = nullptr;

public:
    explicit IconBase(std::shared_ptr<Frame> parent);
    ~IconBase() override = default;

    // Factory metadata: declarations must be available without creating LVGL objects.
    static void RegisterProperties(WidgetPropertyStore&) {}

    void SetAssetsManager(std::shared_ptr<AssetsManager> ui_assets_manager) override;

    void SetProcessingEnabled(bool enabled) override;
    void Configure(std::shared_ptr<WidgetPropertyStore> properties) override;
};

} // namespace eerie_leap::views::widgets::basic::icons
