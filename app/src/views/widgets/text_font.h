#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "views/utilities/font_cache.h"
#include "views/utilities/lvgl_font.h"
#include "views/widgets/widget_property_store.h"

namespace eerie_leap::views::widgets {

using eerie_leap::views::utilities::FontCache;
using eerie_leap::views::utilities::LvglFont;

// FONT_FILE_PATH and FONT_SIZE_PX, for every widget that draws text.
struct TextFont {
    // Both are Rebuild properties: a font is acquired as the widget renders. @p default_size_px is the size the
    // widget's theme font was made at, used when only a file is configured.
    static void RegisterProperties(WidgetPropertyStore& store, int32_t default_size_px);
    // The configured font; nullopt for the theme's (no FONT_FILE_PATH, no cache, or a font that did not load, which
    // the cache logs).
    static std::optional<LvglFont>
    Acquire(const WidgetPropertyStore& properties, const std::shared_ptr<FontCache>& cache);
};

} // namespace eerie_leap::views::widgets
