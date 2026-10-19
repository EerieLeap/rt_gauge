#pragma once

#include <cstdint>
#include <string>
#include <utility>

#include <lvgl.h>

namespace eerie_leap::views::utilities {

struct LvglFont {
private:
    const lv_font_t* lv_font_;

public:
    const std::string name;

    // The size is part of a font's description in the themes, but LVGL's font already carries it.
    LvglFont(std::string name, const lv_font_t* font, uint32_t /*size*/) :
        lv_font_(font),
        name(std::move(name)) {}

    [[nodiscard]] const lv_font_t* ToLvFont() const {
        return lv_font_;
    }
};

} // namespace eerie_leap::views::utilities
