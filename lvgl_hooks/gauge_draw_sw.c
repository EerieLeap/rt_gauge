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

int gauge_blend_rgb565_to_rgb565_swapped(struct _lv_draw_sw_blend_image_dsc_t* dsc) {
    uint8_t* d = dsc->dest_buf;
    const uint8_t* s = dsc->src_buf;
    const int32_t w = dsc->dest_w;

    for(int32_t y = 0; y < dsc->dest_h; y++) {
        const uintptr_t da = (uintptr_t)d;
        const uintptr_t sa = (uintptr_t)s;

        if(((da | sa) & 1) == 0 && ((da ^ sa) & 2) == 0) {
            copy_swap_row((uint16_t*)d, (const uint16_t*)s, w);
        } else {
            /* An odd address (images load with alignment 1) or different 2-byte phases: no
			 * 16-bit or 32-bit access is safe, so LVGL's own two passes do this row. */
            lv_memcpy(d, s, (size_t)w * 2);
            lv_draw_rgb565_swap(d, (uint32_t)w);
        }
        d += dsc->dest_stride;
        s += dsc->src_stride;
    }
    return LV_RESULT_OK;
}
