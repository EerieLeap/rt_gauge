#pragma once

#include <lvgl.h>

#include "subsys/assets/assets_manager.h"
#include "domain/ui_domain/models/icon_type.h"

#include "views/i_renderable.h"
#include "views/widgets/widget_property_store.h"

namespace eerie_leap::views::widgets::basic::icons {

using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::domain::ui_domain::models::IconType;
using eerie_leap::views::widgets::WidgetPropertyStore;

using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::IRenderable;

class IIcon : public virtual IRenderable {
public:
    virtual ~IIcon() = default;

    virtual void SetAssetsManager(std::shared_ptr<AssetsManager> ui_assets_manager) = 0;

    virtual IconType GetIconType() const = 0;

    // Reads the owning widget's store rather than the raw configuration, so the icon and the
    // widget cannot disagree about a property they share.
    virtual void Configure(std::shared_ptr<WidgetPropertyStore> properties) = 0;
    // Processing eligibility is supplied by the owner; it never selects the icon's appearance.
    virtual void SetProcessingEnabled(bool enabled) = 0;
};

} // namespace eerie_leap::views::widgets::basic::icons
