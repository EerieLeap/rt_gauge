# Only builds that choose LVGL's custom SW draw include get this library.
if(CONFIG_LVGL AND CONFIG_LV_DRAW_SW_ASM_CUSTOM)
  zephyr_library()
  zephyr_library_sources(gauge_draw_sw.c)
  zephyr_library_compile_options(-O2)
endif()
