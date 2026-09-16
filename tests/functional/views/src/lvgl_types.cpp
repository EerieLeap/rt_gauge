#include <array>

#include <zephyr/ztest.h>

#include "utilities/type/color.h"
#include "views/utilities/lvgl_color.h"
#include "views/utilities/lvgl_font.h"

using eerie_leap::utilities::type::Color;
using eerie_leap::views::utilities::LvglColor;
using eerie_leap::views::utilities::LvglFont;

ZTEST_SUITE(lvgl_types, NULL, NULL, NULL, NULL, NULL);

ZTEST(lvgl_types, test_parsed_property_color_converts_to_lvgl_with_explicit_alpha) {
    constexpr std::array values { "#12345600", "#12345680", "#123456FF" };
    constexpr std::array<uint8_t, 3> alphas { 0, 128, 255 };
    for(size_t index = 0; index < values.size(); ++index) {
        const auto parsed = Color::TryParse(values[index]);
        zassert_true(parsed.has_value());
        zassert_true(parsed->has_value());
        const auto& color = **parsed;
        LvglColor lvgl_color(color);
        zassert_equal(lvgl_color.r, color.red);
        zassert_equal(lvgl_color.g, color.green);
        zassert_equal(lvgl_color.b, color.blue);
        zassert_equal(lvgl_color.a, alphas[index]);
        zassert_equal(lvgl_color.ToLvOpa(), alphas[index]);
        zassert_equal(lv_color_to_u32(lvgl_color.ToLvColor()), lv_color_to_u32(lv_color_hex(0x123456)));
    }
}

ZTEST(lvgl_types, test_lvgl_color_preserves_existing_rgb_constructors) {
    LvglColor channels(0x12, 0x34, 0x56);
    LvglColor packed(0x123456);
    zassert_equal(channels.ToLvOpa(), 255);
    zassert_equal(packed.ToLvOpa(), 255);
    zassert_equal(lv_color_to_u32(channels.ToLvColor()), lv_color_to_u32(packed.ToLvColor()));
    for(uint8_t alpha : { 0, 128, 255 }) {
        LvglColor rgba(0x12, 0x34, 0x56, alpha);
        LvglColor rgb_alpha(0x123456, alpha);
        zassert_equal(rgba.ToLvOpa(), alpha);
        zassert_equal(rgb_alpha.ToLvOpa(), alpha);
        zassert_equal(lv_color_to_u32(rgba.ToLvColor()), lv_color_to_u32(channels.ToLvColor()));
        zassert_equal(lv_color_to_u32(rgb_alpha.ToLvColor()), lv_color_to_u32(channels.ToLvColor()));
    }
}

ZTEST(lvgl_types, test_lvgl_font_preserves_name_and_font_pointer) {
    const lv_font_t source{};
    LvglFont font("Test", &source, 20);
    zassert_true(font.name == "Test");
    zassert_equal(font.ToLvFont(), &source);
}
