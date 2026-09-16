#pragma once

#include <cstdint>
#include <string>

#include <lvgl.h>

namespace eerie_leap::views::utilities {

struct LvglFont {
private:
    const lv_font_t* lv_font_;

public:
    const std::string name;

    LvglFont(const std::string& name, const lv_font_t* font, uint32_t size)
        : lv_font_(font), name(name) { }

    [[nodiscard]] const lv_font_t* ToLvFont() const {
        return lv_font_;
    }
};

} // namespace eerie_leap::views::utilities
