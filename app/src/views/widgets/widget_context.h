#pragma once

#include <memory>

#include "subsys/assets/assets_manager.h"

#include "domain/ui_domain/services/navigation_service.h"

namespace eerie_leap::views::utilities {
class FontCache;
} // namespace eerie_leap::views::utilities

namespace eerie_leap::views::widgets {

using eerie_leap::domain::ui_domain::services::NavigationService;
using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::views::utilities::FontCache;

// Everything the view layer may hand to a screen or a widget, passed at
// construction instead of a per-dependency setter on IWidget.
struct WidgetContext {
    // The registry the image and animation widgets look their files up in, by kind; null when the unit
    // serves none.
    std::shared_ptr<AssetsManager> assets_manager;
    // The fonts text widgets draw with when configured to; null when the unit serves none.
    std::shared_ptr<FontCache> font_cache;
    std::shared_ptr<NavigationService> navigation_service;
};

} // namespace eerie_leap::views::widgets
