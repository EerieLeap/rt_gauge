# Shared setup for the rt_gauge twister suites.
#
# Must be included before find_package(Zephyr) so that EXTRA_ZEPHYR_MODULES and
# the EXTRA_*_FILE lists are honoured.

get_filename_component(RT_GAUGE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(APP_SRC_DIR "${RT_GAUGE_DIR}/app/src")

set(EXTRA_ZEPHYR_MODULES
    "${RT_GAUGE_DIR}/modules/eerie_leap_rt_core/modules/zephyr_lua"
    "${RT_GAUGE_DIR}/modules/eerie_leap_rt_core/modules/expression_engine"
    "${RT_GAUGE_DIR}/modules/eerie_leap_rt_core")

if(BOARD MATCHES "^qemu_cortex_a")
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/qemu_mmu.conf")
endif()

# CONFIG_MAX_XLAT_TABLES only exists on arm64.
if(BOARD MATCHES "^qemu_cortex_a53")
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/qemu_arm64.conf")
endif()

# qemu_malta only declares 1 MB of SRAM; the machine actually provides far more.
if(BOARD MATCHES "^qemu_malta")
    list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/qemu_malta_ram.overlay")
endif()

# Suites that mount the internal filesystem set RT_GAUGE_TEST_SIM_FLASH before
# including this file. native_sim already ships a simulated flash controller.
if(RT_GAUGE_TEST_SIM_FLASH AND BOARD MATCHES "^qemu_")
    list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/qemu_flash.overlay")
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/qemu_flash.conf")
endif()

# Likewise for the LVGL suites: native_sim has sdl_dc, the QEMU targets do not.
if(RT_GAUGE_TEST_LVGL)
    if(BOARD MATCHES "^qemu_")
        list(APPEND EXTRA_DTC_OVERLAY_FILE "${CMAKE_CURRENT_LIST_DIR}/lvgl_display.overlay")
    endif()
    list(APPEND EXTRA_CONF_FILE "${CMAKE_CURRENT_LIST_DIR}/lvgl_headless.conf")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
