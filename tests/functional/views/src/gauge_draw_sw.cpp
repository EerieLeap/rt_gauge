// The gauge's LVGL blend hook (lvgl_hooks/gauge_draw_sw.c): an opaque RGB565 image copied into an
// RGB565_SWAPPED buffer in one pass. The hook is built only with CONFIG_LV_DRAW_SW_ASM_CUSTOM, so the cases
// below exist only in the scenario that turns it on.
#include <zephyr/ztest.h>

#include <lvgl.h>

#ifdef CONFIG_LV_DRAW_SW_ASM_CUSTOM

    #include <algorithm>
    #include <bit>
    #include <cmath>
    #include <cstdint>
    #include <cstdlib>
    #include <cstring>
    #include <functional>
    #include <vector>

extern "C" {
    #include <lvgl_hooks/gauge_draw_sw.h>
    #include <lvgl_private.h>
}

    #include "views_test_support.h"

namespace {

// The hook and the panels work on bytes of memory and on 16-bit pixels: the only casts between the views.
uint8_t* AsBytes(void* p) {
    return static_cast<uint8_t*>(p);
}

uint16_t* AsPixels(void* p) {
    return static_cast<uint16_t*>(p);
}

const uint16_t* AsPixels(const void* p) {
    return static_cast<const uint16_t*>(p);
}

constexpr uint8_t GUARD = 0xA5;
constexpr size_t MARGIN = 16;

uint8_t SourceByte(size_t i) {
    return static_cast<uint8_t>(i * 37U + 11U);
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
    auto* src_base = AsBytes(src_store.data());
    auto* dst_base = AsBytes(dst_store.data());
    auto* ref_base = AsBytes(ref_store.data());
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
    constexpr int32_t WIDTHS[] = { 1, 2, 3, 15, 16, 17, 466 };
    constexpr int32_t HEIGHTS[] = { 1, 7 };
    for(const int32_t w : WIDTHS) {
        for(const int32_t h : HEIGHTS) {
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
        pixels[i] = static_cast<uint16_t>(0x1234U + i * 0x0111U);
    }
    lv_image_dsc_t image{};
    image.header.magic = LV_IMAGE_HEADER_MAGIC;
    image.header.cf = LV_COLOR_FORMAT_RGB565;
    image.header.w = W;
    image.header.h = H;
    image.header.stride = W * 2;
    image.data_size = pixels.size() * 2;
    image.data = AsBytes(pixels.data());

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

    #ifdef CONFIG_GAUGE_ROUND_PANEL_CLIP

namespace {

// The round panel's corner skip (CONFIG_GAUGE_ROUND_PANEL_CLIP): checked on a real display of the S3's size.
constexpr int32_t PANEL = 466;
constexpr int32_t MARGIN_PX = CONFIG_GAUGE_ROUND_PANEL_CLIP_MARGIN_PX;
constexpr size_t FRAME_BYTES = static_cast<size_t>(PANEL) * PANEL * 2 + 128;

alignas(64) uint8_t frame_store[FRAME_BYTES];
alignas(64) uint8_t other_store[FRAME_BYTES];
uint16_t image_pixels[PANEL * PANEL];

constexpr uint16_t SENTINEL = 0xA55A; // not byte-symmetric, so a swapped sentinel differs from it

uint16_t Swap16(uint16_t v) {
    return std::rotl(v, 8);
}

// RGB565 source pixel; its high byte is always 0x12 so a swapped copy never looks like the sentinel.
uint16_t SourcePixel(int32_t x, int32_t y) {
    return static_cast<uint16_t>(0x1200U | ((x * 5 + y * 3) & 0xFFU));
}

void FillSource() {
    for(int32_t y = 0; y < PANEL; ++y) {
        for(int32_t x = 0; x < PANEL; ++x) {
            image_pixels[y * PANEL + x] = SourcePixel(x, y);
        }
    }
}

void FlushIgnore(lv_display_t* disp, const lv_area_t* /*area*/, uint8_t* /*px_map*/) {
    lv_display_flush_ready(disp);
}

lv_display_t* CreatePanel(uint8_t* second = nullptr) {
    lv_display_t* disp = lv_display_create(PANEL, PANEL);
    zassert_not_null(disp);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
    lv_display_set_buffers(disp, frame_store, second, sizeof(frame_store), LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_flush_cb(disp, FlushIgnore);
    zassert_not_null(disp->buf_act);
    return disp;
}

void FillFrame(lv_display_t* disp, uint16_t value) {
    const lv_draw_buf_t* buf = disp->buf_act;
    for(int32_t y = 0; y < PANEL; ++y) {
        auto* row = AsPixels(buf->data + static_cast<size_t>(y) * buf->header.stride);
        for(int32_t x = 0; x < PANEL; ++x) {
            row[x] = value;
        }
    }
}

// The test's own idea of the pixels a circle inscribed in the PANEL square touches: the pixel's square
// [x, x + 1] x [y, y + 1] has a point nearer than the radius to the centre.
bool TouchesCircle(int32_t x, int32_t y) {
    const double centre = PANEL / 2.0;
    auto nearest = [centre](int32_t p) {
        const double d = std::fabs(p + 0.5 - centre) - 0.5;
        return d > 0 ? d : 0.0;
    };
    const double dx = nearest(x);
    const double dy = nearest(y);
    return dx * dx + dy * dy < centre * centre;
}

// Where the test expects the clipped copy to start in row y: the smallest half-chord in whole pixels that
// covers the row's nearest edge, plus the margin, found by search.
int32_t ExpectedFrom(int32_t y) {
    const int32_t a = std::abs(2 * y + 1 - PANEL);
    const int32_t e = a > 1 ? a - 1 : 0;
    const int32_t v = PANEL * PANEL - e * e;
    int32_t k = 0;
    while(v > 0 && 4 * k * k < v) {
        ++k;
    }
    return std::max<int32_t>(0, (PANEL - 2 * (k + MARGIN_PX)) / 2);
}

struct Rows {
    const uint8_t* data;
    int32_t stride;
    uint16_t At(int32_t x, int32_t y) const {
        return *AsPixels(data + static_cast<size_t>(y) * stride + static_cast<size_t>(x) * 2);
    }
};

// clipped: the copy skipped the corners. Otherwise the first `copied_w` pixels of every row were copied.
void CheckFrame(
    const Rows& rows,
    const std::function<uint16_t(int32_t, int32_t)>& expected,
    bool clipped,
    int32_t copied_w
) {
    for(int32_t y = 0; y < PANEL; ++y) {
        const int32_t from = clipped ? ExpectedFrom(y) : 0;
        const int32_t end = clipped ? PANEL - from : copied_w;
        for(int32_t x = 0; x < PANEL; ++x) {
            const uint16_t got = rows.At(x, y);
            if(x >= from && x < end) {
                zassert_equal(got, expected(x, y), "pixel %d,%d is 0x%04x", (int)x, (int)y, (unsigned)got);
            } else if(clipped) {
                zassert_false(TouchesCircle(x, y), "visible pixel %d,%d outside the copied range", (int)x, (int)y);
                zassert_equal(got, SENTINEL, "pixel %d,%d outside the chord was written", (int)x, (int)y);
            } else {
                zassert_equal(got, SENTINEL, "pixel %d,%d beyond the copied width was written", (int)x, (int)y);
            }
        }
    }
}

void CallHook(
    uint8_t* dest,
    int32_t stride,
    int32_t w,
    int32_t h,
    int32_t x1,
    const uint8_t* src = AsBytes(image_pixels)
) {
    _lv_draw_sw_blend_image_dsc_t dsc{};
    dsc.dest_buf = dest;
    dsc.dest_w = w;
    dsc.dest_h = h;
    dsc.dest_stride = stride;
    dsc.relative_area = { x1, 0, x1 + w - 1, h - 1 };
    dsc.src_buf = src;
    dsc.src_stride = PANEL * 2;
    dsc.src_color_format = LV_COLOR_FORMAT_RGB565;
    dsc.opa = LV_OPA_COVER;
    dsc.blend_mode = LV_BLEND_MODE_NORMAL;
    zassert_equal(gauge_blend_rgb565_to_rgb565_swapped(&dsc), LV_RESULT_OK);
}

uint16_t SwappedSource(int32_t x, int32_t y) {
    return Swap16(SourcePixel(x, y));
}

void FillBytes(uint8_t* base, size_t bytes) {
    for(size_t i = 0; i + 1 < bytes; i += 2) {
        *AsPixels(base + i) = SENTINEL;
    }
}

} // namespace

ZTEST(gauge_draw_sw, test_full_frame_blit_skips_only_the_corners_and_other_blits_copy_everything) {
    views_test::EnsureTestDisplay();
    FillSource();
    lv_display_t* disp = CreatePanel();
    const auto stride = static_cast<int32_t>(disp->buf_act->header.stride);
    uint8_t* frame = disp->buf_act->data;

    // All five conditions hold.
    FillFrame(disp, SENTINEL);
    lv_refr_set_disp_refreshing(disp);
    CallHook(frame, stride, PANEL, PANEL, 0);
    CheckFrame({ frame, stride }, SwappedSource, true, PANEL);

    // Each broken in turn: every pixel of every row is copied.
    FillFrame(disp, SENTINEL);
    FillBytes(other_store, sizeof(other_store));
    {
        auto* other = &other_store[0];
        // Not the active buffer (same size): the stride of the buffer the display made, 64-aligned.
        CallHook(other, stride, PANEL, PANEL, 0);
        CheckFrame({ other, stride }, SwappedSource, false, PANEL);
    }

    FillFrame(disp, SENTINEL);
    CallHook(frame, stride, PANEL, PANEL, 1); // relative_area not at the origin
    CheckFrame({ frame, stride }, SwappedSource, false, PANEL);

    FillFrame(disp, SENTINEL);
    CallHook(frame, stride, PANEL - 1, PANEL, 0); // narrower
    CheckFrame({ frame, stride }, SwappedSource, false, PANEL - 1);

    FillFrame(disp, SENTINEL);
    CallHook(frame, stride, PANEL, PANEL - 1, 0); // shorter: the last row stays
    for(int32_t x = 0; x < PANEL; ++x) {
        zassert_equal(Rows({ frame, stride }).At(x, PANEL - 1), SENTINEL);
    }
    {
        // Rows 0..PANEL-2 were copied whole.
        const Rows rows{ frame, stride };
        for(int32_t y = 0; y < PANEL - 1; ++y) {
            for(int32_t x = 0; x < PANEL; ++x) {
                zassert_equal(rows.At(x, y), SwappedSource(x, y), "pixel %d,%d", (int)x, (int)y);
            }
        }
    }

    FillFrame(disp, SENTINEL);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);
    CallHook(frame, stride, PANEL, PANEL, 0);
    lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_0);
    CheckFrame({ frame, stride }, SwappedSource, false, PANEL);

    FillFrame(disp, SENTINEL);
    lv_refr_set_disp_refreshing(nullptr);
    CallHook(frame, stride, PANEL, PANEL, 0);
    CheckFrame({ frame, stride }, SwappedSource, false, PANEL);

    lv_refr_set_disp_refreshing(nullptr);
    lv_display_delete(disp);
}

ZTEST(gauge_draw_sw, test_a_full_screen_image_refresh_leaves_the_corners_alone) {
    views_test::EnsureTestDisplay();
    FillSource();
    lv_display_t* disp = CreatePanel();

    lv_image_dsc_t image{};
    image.header.magic = LV_IMAGE_HEADER_MAGIC;
    image.header.cf = LV_COLOR_FORMAT_RGB565;
    image.header.w = PANEL;
    image.header.h = PANEL;
    image.header.stride = PANEL * 2;
    image.data_size = sizeof(image_pixels);
    image.data = AsBytes(image_pixels);

    lv_obj_t* screen = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_opa(screen, LV_OPA_TRANSP, 0);
    lv_obj_t* img = lv_image_create(screen);
    lv_image_set_src(img, &image);
    lv_obj_set_pos(img, 0, 0);

    FillFrame(disp, SENTINEL);
    lv_refr_now(disp);

    const lv_draw_buf_t* buf = disp->buf_act;
    CheckFrame({ buf->data, static_cast<int32_t>(buf->header.stride) }, SwappedSource, true, PANEL);

    lv_obj_delete(img);
    lv_display_delete(disp);
}

// The clipped copy on the fallback path: the source starts 1 byte (odd address) or 2 bytes (other 4-byte
// phase than the destination) off, so the row is copied by lv_memcpy and lv_draw_rgb565_swap.
ZTEST(gauge_draw_sw, test_the_clipped_copy_is_exact_on_the_fallback_path) {
    views_test::EnsureTestDisplay();
    lv_display_t* disp = CreatePanel();
    const auto stride = static_cast<int32_t>(disp->buf_act->header.stride);
    uint8_t* frame = disp->buf_act->data;
    alignas(4) static uint8_t src_store[PANEL * PANEL * 2 + 8];

    for(const size_t offset : { 1U, 2U }) {
        for(int32_t y = 0; y < PANEL; ++y) {
            for(int32_t x = 0; x < PANEL; ++x) {
                const uint16_t px = SourcePixel(x, y);
                memcpy(src_store + offset + (static_cast<size_t>(y) * PANEL + x) * 2, &px, sizeof(px));
            }
        }
        FillFrame(disp, SENTINEL);
        lv_refr_set_disp_refreshing(disp);
        CallHook(frame, stride, PANEL, PANEL, 0, src_store + offset);
        lv_refr_set_disp_refreshing(nullptr);
        CheckFrame({ frame, stride }, SwappedSource, true, PANEL);
    }
    lv_display_delete(disp);
}

// The S3 renders into two buffers in turn: the corner skip must hold for the one that is not the first.
ZTEST(gauge_draw_sw, test_the_corners_are_skipped_in_both_buffers_of_a_double_buffered_panel) {
    views_test::EnsureTestDisplay();
    FillSource();
    lv_display_t* disp = CreatePanel(other_store);

    lv_image_dsc_t image{};
    image.header.magic = LV_IMAGE_HEADER_MAGIC;
    image.header.cf = LV_COLOR_FORMAT_RGB565;
    image.header.w = PANEL;
    image.header.h = PANEL;
    image.header.stride = PANEL * 2;
    image.data_size = sizeof(image_pixels);
    image.data = AsBytes(image_pixels);

    lv_obj_t* screen = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_opa(screen, LV_OPA_TRANSP, 0);
    lv_obj_t* img = lv_image_create(screen);
    lv_image_set_src(img, &image);
    lv_obj_set_pos(img, 0, 0);

    const uint8_t* seen[4] = {};
    for(const uint8_t*& frame : seen) {
        // Both buffers start as sentinel; the refresh renders into one of them.
        FillBytes(frame_store, sizeof(frame_store));
        FillBytes(other_store, sizeof(other_store));
        lv_obj_invalidate(screen);
        lv_refr_now(disp);
        const lv_draw_buf_t* target = disp->buf_1;
        if(Rows({ target->data, static_cast<int32_t>(target->header.stride) }).At(PANEL / 2, PANEL / 2) == SENTINEL) {
            target = disp->buf_2;
        }
        frame = target->data;
        CheckFrame({ target->data, static_cast<int32_t>(target->header.stride) }, SwappedSource, true, PANEL);
    }
    zassert_not_equal(seen[0], seen[1], "the panel did not alternate its buffers");

    lv_obj_delete(img);
    lv_display_delete(disp);
}

// A nearly opaque image is drawn through a layer of strips as wide as the screen (466x26, never the
// display's height), blended by the same hook into the layer's buffer. Their corners must reach the frame.
// This does not isolate the destination-buffer guard (the direct-call case and the canvas case do).
ZTEST(gauge_draw_sw, test_a_layered_full_width_image_keeps_its_corners) {
    views_test::EnsureTestDisplay();
    constexpr uint16_t RED = 0xF800;
    for(auto& px : image_pixels) {
        px = RED;
    }
    lv_display_t* disp = CreatePanel();

    lv_image_dsc_t image{};
    image.header.magic = LV_IMAGE_HEADER_MAGIC;
    image.header.cf = LV_COLOR_FORMAT_RGB565;
    image.header.w = PANEL;
    image.header.h = PANEL;
    image.header.stride = PANEL * 2;
    image.data_size = sizeof(image_pixels);
    image.data = AsBytes(image_pixels);

    lv_obj_t* screen = lv_display_get_screen_active(disp);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x0000FF), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_t* img = lv_image_create(screen);
    lv_image_set_src(img, &image);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_set_style_opa_layered(img, LV_OPA_COVER - 1, 0);

    FillFrame(disp, SENTINEL);
    lv_refr_now(disp);

    const lv_draw_buf_t* buf = disp->buf_act;
    const Rows rows{ buf->data, static_cast<int32_t>(buf->header.stride) };
    for(const int32_t x : { 0, 2, 233, 463, 465 }) {
        for(const int32_t y : { 0, 2, 233, 463, 465 }) {
            // Red with a trace of the blue background behind it: R5 31 or 30, no green, B5 at most 1.
            const uint16_t px = Swap16(rows.At(x, y));
            zassert_true(
                (px >> 11) >= 30 && ((px >> 5) & 0x3F) == 0 && (px & 0x1F) <= 1, "pixel %d,%d is 0x%04x", (int)x,
                (int)y, (unsigned)px
            );
        }
    }

    lv_obj_delete(img);
    lv_display_delete(disp);
}

// A canvas of the display's size in the display's format is drawn while the display is refreshing (as a
// snapshot is): it is not the active buffer, so every pixel of it is copied, corners included.
ZTEST(gauge_draw_sw, test_a_display_sized_canvas_is_copied_whole_while_the_display_refreshes) {
    views_test::EnsureTestDisplay();
    FillSource();
    lv_display_t* disp = CreatePanel();

    lv_draw_buf_t canvas_buf;
    lv_draw_buf_init(&canvas_buf, PANEL, PANEL, LV_COLOR_FORMAT_RGB565_SWAPPED, 0, other_store, sizeof(other_store));
    lv_draw_buf_clear(&canvas_buf, nullptr);
    FillBytes(other_store, sizeof(other_store));
    zassert_not_equal(static_cast<const void*>(canvas_buf.data), static_cast<const void*>(disp->buf_act->data));

    lv_image_dsc_t image{};
    image.header.magic = LV_IMAGE_HEADER_MAGIC;
    image.header.cf = LV_COLOR_FORMAT_RGB565;
    image.header.w = PANEL;
    image.header.h = PANEL;
    image.header.stride = PANEL * 2;
    image.data_size = sizeof(image_pixels);
    image.data = AsBytes(image_pixels);

    lv_obj_t* canvas = lv_canvas_create(lv_display_get_screen_active(disp));
    lv_canvas_set_draw_buf(canvas, &canvas_buf);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src = &image;
    const lv_area_t area = { 0, 0, PANEL - 1, PANEL - 1 };
    lv_refr_set_disp_refreshing(disp);
    lv_draw_image(&layer, &dsc, &area);
    lv_canvas_finish_layer(canvas, &layer);
    lv_refr_set_disp_refreshing(nullptr);

    CheckFrame({ canvas_buf.data, static_cast<int32_t>(canvas_buf.header.stride) }, SwappedSource, false, PANEL);

    lv_obj_delete(canvas);
    lv_display_delete(disp);
}

    #endif // CONFIG_GAUGE_ROUND_PANEL_CLIP

ZTEST_SUITE(gauge_draw_sw, nullptr, nullptr, nullptr, views_test::CleanTestDisplay, nullptr);

#endif // CONFIG_LV_DRAW_SW_ASM_CUSTOM
