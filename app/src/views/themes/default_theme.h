#pragma once

#include "views/themes/i_theme.h"

namespace eerie_leap::views::themes {

// using namespace eerie_leap::views::assets::fonts;

class DefaultTheme : public ITheme {
public:
    LvglColor GetPrimaryColor() const override {
        return { 0x0F0F0F }; // Dark gray
    }

    LvglColor GetSecondaryColor() const override {
        return { 0x1BBE5F }; // Green
    }

    LvglColor GetInactiveColor() const override {
        return { 0x919191 }; // Medium gray
    }

    LvglColor GetBackgroundColor() const override {
        return { 0xFFFFFF }; // White
    }

    LvglColor GetSurfaceColor() const override {
        return { 0xF5F5F5 }; // Light gray
    }

    LvglColor GetAccentColor() const override {
        return { 0xF56060 }; // Red
    }

    LvglColor GetErrorColor() const override {
        return { 0xF44336 }; // Red
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
