/* LVGL custom SW-ASM include (CONFIG_LV_DRAW_SW_ASM_CUSTOM_INCLUDE). LVGL includes it from lv_conf_internal.h too,
 * before its own types, so it declares only what it needs. The function lives in lvgl_hooks/gauge_draw_sw.c. */
#ifndef GAUGE_DRAW_SW_H
#define GAUGE_DRAW_SW_H

struct _lv_draw_sw_blend_image_dsc_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Returns LV_RESULT_OK (1) when it drew, LV_RESULT_INVALID (0) to fall back to LVGL's loop. */
int gauge_blend_rgb565_to_rgb565_swapped(struct _lv_draw_sw_blend_image_dsc_t* dsc);

#ifdef __cplusplus
}
#endif

#define LV_DRAW_SW_RGB565_BLEND_NORMAL_TO_RGB565_SWAPPED(dsc) gauge_blend_rgb565_to_rgb565_swapped(dsc)

#endif
