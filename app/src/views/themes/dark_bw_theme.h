#pragma once

#include "views/themes/i_theme.h"

namespace eerie_leap::views::themes {

class DarkBWTheme : public ITheme {
public:
    LvglColor GetPrimaryColor() const override {
        return LvglColor(0xFFFFFF); // White
    }

    LvglColor GetSecondaryColor() const override {
        return LvglColor(0xFAFAFA); // Light gray
    }

    LvglColor GetInactiveColor() const override {
        return LvglColor(0x909090); // Light gray
    }

    LvglColor GetBackgroundColor() const override {
        return LvglColor(0x000000); // Black
    }

    LvglColor GetSurfaceColor() const override {
        return LvglColor(0x1E1E1E); // Dark gray
    }

    LvglColor GetAccentColor() const override {
        return LvglColor(0xFFFFFF); // White
    }

    LvglColor GetErrorColor() const override {
        return LvglColor(0xFFFFFF); // White
    }

    LvglFont GetPrimaryFont() const override {
        return LvglFont("Montserrat", &lv_font_rubik_medium_20, 20);
    }

    LvglFont GetSecondaryFont() const override {
        return LvglFont("Montserrat", &lv_font_montserrat_20, 20);
    }

    LvglFont GetPrimaryFontLarge() const override {
        return LvglFont("Inconsolata", &lv_font_inconsolata_bold_120, 120);
    }
};

} // namespace eerie_leap::views::themes
