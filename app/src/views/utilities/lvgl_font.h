#pragma once

#include <memory>

#include <lvgl.h>

namespace eerie_leap::views::utilities {

// A font as widgets hand it to LVGL: a compiled font, or one FontCache loaded, which it keeps alive.
class LvglFont {
private:
    std::shared_ptr<const lv_font_t> lv_font_;

public:
    // A compiled font: static, so nothing is owned or counted.
    explicit LvglFont(const lv_font_t* font);
    // A loaded font, freed by its deleter when the last copy goes.
    explicit LvglFont(std::shared_ptr<const lv_font_t> font);

    [[nodiscard]] const lv_font_t* ToLvFont() const;

    // Keeps the font alive until @p object is deleted, whatever order its owner releases things in. No effect for a
    // compiled font.
    void BindTo(lv_obj_t* object) const;
};

} // namespace eerie_leap::views::utilities
