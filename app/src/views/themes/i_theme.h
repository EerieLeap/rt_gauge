#pragma once

#include "views/utilities/lvgl_color.h"
#include "views/utilities/lvgl_font.h"
#include "views/assets/fonts/fonts_register.h"

namespace eerie_leap::views::themes {

using eerie_leap::views::utilities::LvglColor;
using eerie_leap::views::utilities::LvglFont;

class ITheme {
public:
    virtual ~ITheme() = default;

    // Colors
    [[nodiscard]] virtual LvglColor GetPrimaryColor() const = 0;
    [[nodiscard]] virtual LvglColor GetSecondaryColor() const = 0;
    [[nodiscard]] virtual LvglColor GetInactiveColor() const = 0;
    [[nodiscard]] virtual LvglColor GetBackgroundColor() const = 0;
    [[nodiscard]] virtual LvglColor GetSurfaceColor() const = 0;
    [[nodiscard]] virtual LvglColor GetAccentColor() const = 0;
    [[nodiscard]] virtual LvglColor GetErrorColor() const = 0;

    [[nodiscard]] virtual LvglFont GetPrimaryFont() const = 0;
    [[nodiscard]] virtual LvglFont GetSecondaryFont() const = 0;
    [[nodiscard]] virtual LvglFont GetPrimaryFontLarge() const = 0;
};

} // namespace eerie_leap::views::themes
