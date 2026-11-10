zephyr_syscall_include_directories(include)
zephyr_include_directories(include)

add_subdirectory(drivers)

# LVGL hooks, built only with CONFIG_LV_DRAW_SW_ASM_CUSTOM.
add_subdirectory(lvgl_hooks)
