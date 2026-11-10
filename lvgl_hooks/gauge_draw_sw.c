/* LVGL draw hooks for the gauge. Copying an opaque RGB565 image into an RGB565_SWAPPED buffer takes LVGL a
 * memcpy and then a second pass over the row; here the bytes are swapped while they are copied, so each source
 * and destination line is touched once (the D-cache allocates on a write miss in PSRAM, so passes are costly). */
#include <stdint.h>

#include <lvgl.h>
#include <lvgl_private.h>

#define SWAP32(v) ((((v) & 0xff00ff00u) >> 8) | (((v) & 0x00ff00ffu) << 8))
#define SWAP16(v) ((uint16_t)((((v) & 0xff00u) >> 8) | (((v) & 0x00ffu) << 8)))

/* Both pointers are 2-byte aligned and equal modulo 4. */
static void copy_swap_row(uint16_t* d16, const uint16_t* s16, int32_t n) {
    if(((uintptr_t)d16 & 2) && n > 0) {
        *d16++ = SWAP16(*s16);
        s16++;
        n--;
    }

    uint32_t* d32 = (uint32_t*)d16;
    const uint32_t* s32 = (const uint32_t*)s16;
    int32_t words = n / 2;

    while(words >= 8) {
        uint32_t a = s32[0];
        uint32_t b = s32[1];
        uint32_t c = s32[2];
        uint32_t e = s32[3];
        uint32_t f = s32[4];
        uint32_t g = s32[5];
        uint32_t h = s32[6];
        uint32_t i = s32[7];

        d32[0] = SWAP32(a);
        d32[1] = SWAP32(b);
        d32[2] = SWAP32(c);
        d32[3] = SWAP32(e);
        d32[4] = SWAP32(f);
        d32[5] = SWAP32(g);
        d32[6] = SWAP32(h);
        d32[7] = SWAP32(i);
        d32 += 8;
        s32 += 8;
        words -= 8;
    }
    while(words-- > 0) {
        *d32++ = SWAP32(*s32);
        s32++;
    }
    if(n & 1) {
        *(uint16_t*)d32 = SWAP16(*(const uint16_t*)s32);
    }
}

#ifdef CONFIG_GAUGE_ROUND_PANEL_CLIP
/* True for a blit that fills the display's whole active frame buffer, the only one whose corners are not
 * on screen: a layer of the same size (a transform layer, a snapshot) is shown later, scaled or moved. */
static int is_full_frame_blit(const lv_draw_sw_blend_image_dsc_t* dsc) {
    lv_display_t* disp = lv_refr_get_disp_refreshing();

    return disp != NULL
        && disp->buf_act != NULL
        && dsc->dest_buf == disp->buf_act->data
        && dsc->relative_area.x1 == 0
        && dsc->relative_area.y1 == 0
        && dsc->dest_w == disp->hor_res
        && dsc->dest_h == disp->ver_res
        && lv_display_get_rotation(disp) == LV_DISPLAY_ROTATION_0;
}
#endif

int gauge_blend_rgb565_to_rgb565_swapped(struct _lv_draw_sw_blend_image_dsc_t* dsc) {
    uint8_t* d = dsc->dest_buf;
    const uint8_t* s = dsc->src_buf;
    const int32_t w = dsc->dest_w;

#ifdef CONFIG_GAUGE_ROUND_PANEL_CLIP
    /* With both sides at most 32767, every product below stays under 2^31 (32769^2 = 1,073,807,361 < 2^31). */
    const int clip = dsc->dest_w > 0
        && dsc->dest_h > 0
        && dsc->dest_w <= 32767
        && dsc->dest_h <= 32767
        && is_full_frame_blit(dsc);
    const int32_t h = dsc->dest_h;
    const int32_t diameter = w < h ? w : h;
    int32_t k = 0;
#endif

    for(int32_t y = 0; y < dsc->dest_h; y++) {
        int32_t from = 0;
        int32_t n = w;

#ifdef CONFIG_GAUGE_ROUND_PANEL_CLIP
        if(clip) {
            /* Doubled coordinates: a is the distance from the circle's centre to the row's centre, e the one
             * to the row's nearest edge (a pixel the circle only touches at that edge is still copied). */
            int32_t a = 2 * y + 1 - h;
            if(a < 0)
                a = -a;
            const int32_t e = a > 1 ? a - 1 : 0;
            const int32_t v = diameter * diameter - e * e;
            if(v <= 0) {
                k = 0;
            } else {
                /* k: the smallest integer with (2k)^2 >= v; it moves by one pixel or two a row, so
                 * the loops cost about the diameter over a frame, with no square root. */
                while((2 * k) * (2 * k) < v)
                    k++;
                while(k > 0 && (2 * (k - 1)) * (2 * (k - 1)) >= v)
                    k--;
            }
            from = (w - 2 * (k + CONFIG_GAUGE_ROUND_PANEL_CLIP_MARGIN_PX)) / 2;
            if(from < 0)
                from = 0;
            n = w - 2 * from;
        }
#endif

        uint8_t* dr = d + (size_t)from * 2;
        const uint8_t* sr = s + (size_t)from * 2;
        const uintptr_t da = (uintptr_t)dr;
        const uintptr_t sa = (uintptr_t)sr;

        if(((da | sa) & 1) == 0 && ((da ^ sa) & 2) == 0) {
            copy_swap_row((uint16_t*)dr, (const uint16_t*)sr, n);
        } else {
            /* An odd address (images load with alignment 1) or different 2-byte phases: no
			 * 16-bit or 32-bit access is safe, so LVGL's own two passes do this row. */
            lv_memcpy(dr, sr, (size_t)n * 2);
            lv_draw_rgb565_swap(dr, (uint32_t)n);
        }
        d += dsc->dest_stride;
        s += dsc->src_stride;
    }
    return LV_RESULT_OK;
}
