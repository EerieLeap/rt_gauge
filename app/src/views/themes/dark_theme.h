#pragma once

#include "views/themes/i_theme.h"

namespace eerie_leap::views::themes {

class DarkTheme : public ITheme {
public:
    LvglColor GetPrimaryColor() const override {
        return { 0xFAFAFA }; // Light gray
    }

    LvglColor GetSecondaryColor() const override {
        return { 0x1BBE5F }; // Green
    }

    LvglColor GetInactiveColor() const override {
        return { 0x909090 }; // Light gray
    }

    LvglColor GetBackgroundColor() const override {
        return { 0x000000 }; // Black
    }

    LvglColor GetSurfaceColor() const override {
        return { 0x1E1E1E }; // Dark gray
    }

    LvglColor GetAccentColor() const override {
        return { 0xF56060 }; // Red
    }

    LvglColor GetErrorColor() const override {
        return { 0xEF5350 }; // Light red
    }

    LvglFont GetPrimaryFont() const override {
        return { "Montserrat", &lv_font_rubik_medium_20, 20 };
    }

    LvglFont GetSecondaryFont() const override {
        return { "Montserrat", &lv_font_montserrat_20, 20 };
    }

    LvglFont GetPrimaryFontLarge() const override {
        return { "Inconsolata", &lv_font_inconsolata_bold_120, 120 };
    }
};

} // namespace eerie_leap::views::themes
