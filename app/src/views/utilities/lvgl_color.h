#pragma once

#include <cstdint>

#include <lvgl.h>

#include "utilities/type/color.h"

namespace eerie_leap::views::utilities {

using eerie_leap::utilities::type::Color;

struct LvglColor {
private:
    lv_color_t lv_color_;

public:
    const uint8_t r;
    const uint8_t g;
    const uint8_t b;
    const uint8_t a;

    LvglColor(uint8_t red, uint8_t green, uint8_t blue, uint8_t alpha = 255)
        : lv_color_(lv_color_make(red, green, blue)), r(red), g(green), b(blue), a(alpha) { }

    LvglColor(uint32_t rgb, uint8_t alpha = 255)
        : lv_color_(lv_color_hex(rgb)), r(rgb >> 16), g(rgb >> 8), b(rgb), a(alpha) { }

    explicit LvglColor(const Color& color)
        : LvglColor(color.red, color.green, color.blue, color.alpha) { }

    [[nodiscard]] lv_color_t ToLvColor() const {
        return lv_color_;
    }

    [[nodiscard]] lv_opa_t ToLvOpa() const {
        return a;
    }
};

} // namespace eerie_leap::views::utilities
