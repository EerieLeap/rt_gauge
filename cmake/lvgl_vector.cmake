# Zephyr's LVGL module collects C sources only. Build the bundled ThorVG C++
# renderer on the same target so it inherits LVGL's configuration and allocator.

# Tests do not run app/CMakeLists.txt's patch step: apply every LVGL patch here (idempotent),
# so the suites build the LVGL the application does, on a fresh checkout too.
include(${CMAKE_CURRENT_LIST_DIR}/lvgl_patches.cmake)
if(ZEPHYR_LVGL_MODULE_DIR)
    rt_gauge_apply_lvgl_patches("${ZEPHYR_LVGL_MODULE_DIR}")
endif()

if(CONFIG_LV_USE_VECTOR_GRAPHIC AND CONFIG_LV_USE_THORVG_INTERNAL)
    file(GLOB THORVG_SOURCES CONFIGURE_DEPENDS
        "${ZEPHYR_LVGL_MODULE_DIR}/src/libs/thorvg/*.cpp")
    # Zephyr links modules with --whole-archive. Even unused loader translation
    # units can retain iostream initialization and pull in POSIX file functions.
    # The SVG loader is never enabled (see lvgl_thorvg_optional_loaders.patch).
    list(FILTER THORVG_SOURCES EXCLUDE REGEX "/tvgSvg[^/]*\\.cpp$")
    if(NOT CONFIG_LV_USE_LOTTIE)
        list(FILTER THORVG_SOURCES EXCLUDE REGEX "/tvgLottie[^/]*\\.cpp$")
    endif()
    target_sources(modules__lvgl PRIVATE ${THORVG_SOURCES})
endif()
