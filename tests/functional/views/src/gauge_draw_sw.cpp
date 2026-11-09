// The gauge's LVGL blend hook (lvgl_hooks/gauge_draw_sw.c): an opaque RGB565 image copied into an
// RGB565_SWAPPED buffer in one pass. The hook is built only with CONFIG_LV_DRAW_SW_ASM_CUSTOM, so the cases
// below exist only in the scenario that turns it on.
#include <zephyr/ztest.h>

#include <lvgl.h>

#ifdef CONFIG_LV_DRAW_SW_ASM_CUSTOM

    #include <cstdint>
    #include <cstring>
    #include <vector>

extern "C" {
    #include <lvgl_hooks/gauge_draw_sw.h>
    #include <lvgl_private.h>
}

    #include "views_test_support.h"

namespace {

constexpr uint8_t GUARD = 0xA5;
constexpr size_t MARGIN = 16;

uint8_t SourceByte(size_t i) {
    return static_cast<uint8_t>(i * 37u + 11u);
}

// Runs the hook on a source at byte offset src_off and a destination at dst_off, then compares every byte
// of the destination buffer, guards included, with what lv_memcpy + lv_draw_rgb565_swap leaves.
void CheckBlend(int32_t w, int32_t h, size_t src_off, size_t dst_off, int32_t src_extra, int32_t dst_extra) {
    const int32_t src_stride = w * 2 + src_extra;
    const int32_t dst_stride = w * 2 + dst_extra;
    // Backing store of 4-byte alignment, so an offset is the pointer's real offset modulo 4.
    std::vector<uint32_t> src_store((MARGIN + src_stride * h + 16) / 4 + 1);
    std::vector<uint32_t> dst_store((MARGIN + dst_stride * h + 16) / 4 + 1);
    std::vector<uint32_t> ref_store(dst_store.size());
    auto* src_base = reinterpret_cast<uint8_t*>(src_store.data());
    auto* dst_base = reinterpret_cast<uint8_t*>(dst_store.data());
    auto* ref_base = reinterpret_cast<uint8_t*>(ref_store.data());
    const size_t dst_bytes = dst_store.size() * 4;

    for(size_t i = 0; i < src_store.size() * 4; ++i) {
        src_base[i] = SourceByte(i);
    }
    std::memset(dst_base, GUARD, dst_bytes);
    std::memset(ref_base, GUARD, dst_bytes);

    const uint8_t* src = src_base + MARGIN + src_off;
    uint8_t* dst = dst_base + MARGIN + dst_off;
    uint8_t* ref = ref_base + MARGIN + dst_off;

    for(int32_t y = 0; y < h; ++y) {
        lv_memcpy(ref + y * dst_stride, src + y * src_stride, static_cast<size_t>(w) * 2);
        lv_draw_rgb565_swap(ref + y * dst_stride, static_cast<uint32_t>(w));
    }

    _lv_draw_sw_blend_image_dsc_t dsc{};
    dsc.dest_buf = dst;
    dsc.dest_w = w;
    dsc.dest_h = h;
    dsc.dest_stride = dst_stride;
    dsc.src_buf = src;
    dsc.src_stride = src_stride;
    dsc.src_color_format = LV_COLOR_FORMAT_RGB565;
    dsc.opa = LV_OPA_COVER;
    dsc.blend_mode = LV_BLEND_MODE_NORMAL;

    zassert_equal(gauge_blend_rgb565_to_rgb565_swapped(&dsc), LV_RESULT_OK);
    for(size_t i = 0; i < dst_bytes; ++i) {
        if(dst_base[i] != ref_base[i]) {
            zassert_true(
                false, "w %d h %d src_off %u dst_off %u src_extra %d dst_extra %d: byte %u is 0x%02x, expected 0x%02x",
                (int)w, (int)h, (unsigned)src_off, (unsigned)dst_off, (int)src_extra, (int)dst_extra, (unsigned)i,
                dst_base[i], ref_base[i]
            );
        }
    }
}

} // namespace

ZTEST(gauge_draw_sw, test_hook_matches_memcpy_and_swap_for_every_alignment_and_size) {
    constexpr int32_t widths[] = { 1, 2, 3, 15, 16, 17, 466 };
    constexpr int32_t heights[] = { 1, 7 };
    for(const int32_t w : widths) {
        for(const int32_t h : heights) {
            for(size_t src_off = 0; src_off < 4; ++src_off) {
                for(size_t dst_off = 0; dst_off < 4; ++dst_off) {
                    CheckBlend(w, h, src_off, dst_off, 0, 0);
                    // Strides wider than the row, in bytes; 6 keeps the row start phase changing per row.
                    CheckBlend(w, h, src_off, dst_off, 8, 8);
                    CheckBlend(w, h, src_off, dst_off, 0, 6);
                    CheckBlend(w, h, src_off, dst_off, 6, 0);
                }
            }
        }
    }
}

ZTEST(gauge_draw_sw, test_rgb565_image_reaches_a_swapped_canvas_as_big_endian) {
    views_test::EnsureTestDisplay();

    constexpr int32_t W = 17;
    constexpr int32_t H = 5;
    constexpr int32_t X = 1; // an odd pixel column: the destination row starts on a 2-byte phase
    std::vector<uint16_t> pixels(W * H);
    for(size_t i = 0; i < pixels.size(); ++i) {
        pixels[i] = static_cast<uint16_t>(0x1234u + i * 0x0111u);
    }
    lv_image_dsc_t image{};
    image.header.magic = LV_IMAGE_HEADER_MAGIC;
    image.header.cf = LV_COLOR_FORMAT_RGB565;
    image.header.w = W;
    image.header.h = H;
    image.header.stride = W * 2;
    image.data_size = pixels.size() * 2;
    image.data = reinterpret_cast<const uint8_t*>(pixels.data());

    constexpr int32_t CANVAS_W = 24;
    constexpr int32_t CANVAS_H = 8;
    lv_draw_buf_t* buf = lv_draw_buf_create(CANVAS_W, CANVAS_H, LV_COLOR_FORMAT_RGB565_SWAPPED, 0);
    zassert_not_null(buf);
    lv_obj_t* canvas = lv_canvas_create(lv_screen_active());
    lv_canvas_set_draw_buf(canvas, buf);
    lv_canvas_fill_bg(canvas, lv_color_black(), LV_OPA_COVER);

    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = &image;
    const lv_area_t area = { X, 1, X + W - 1, H };
    lv_draw_image(&layer, &dsc, &area);
    lv_canvas_finish_layer(canvas, &layer);

    for(int32_t y = 0; y < H; ++y) {
        const uint8_t* row = static_cast<const uint8_t*>(lv_draw_buf_goto_xy(buf, 0, 1 + y));
        for(int32_t x = 0; x < W; ++x) {
            const uint16_t expected = pixels[y * W + x];
            zassert_equal(row[(X + x) * 2], expected >> 8, "high byte at %d,%d", (int)x, (int)y);
            zassert_equal(row[(X + x) * 2 + 1], expected & 0xFF, "low byte at %d,%d", (int)x, (int)y);
        }
    }

    lv_obj_delete(canvas);
    lv_draw_buf_destroy(buf);
}

ZTEST_SUITE(gauge_draw_sw, nullptr, nullptr, nullptr, views_test::CleanTestDisplay, nullptr);

#endif // CONFIG_LV_DRAW_SW_ASM_CUSTOM
