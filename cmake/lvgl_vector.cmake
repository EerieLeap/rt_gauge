# Zephyr's LVGL module collects C sources only. Build the bundled ThorVG C++
# renderer on the same target so it inherits LVGL's configuration and allocator.
if(CONFIG_LV_USE_VECTOR_GRAPHIC AND CONFIG_LV_USE_THORVG_INTERNAL)
    # Tests do not run app/CMakeLists.txt's patch step. Apply the same fixes here
    # so every target using ThorVG gets them, including a fresh checkout.
    foreach(THORVG_PATCH_NAME IN ITEMS thorvg_raster_workspace thorvg_optional_loaders
            lottie_straight_alpha)
        set(THORVG_PATCH
            "${CMAKE_CURRENT_LIST_DIR}/../app/patches/modules/lvgl/lvgl_${THORVG_PATCH_NAME}.patch")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${THORVG_PATCH}")
        execute_process(COMMAND git apply --reverse --check "${THORVG_PATCH}"
            WORKING_DIRECTORY "${ZEPHYR_LVGL_MODULE_DIR}"
            RESULT_VARIABLE THORVG_PATCH_APPLIED OUTPUT_QUIET ERROR_QUIET)
        if(NOT THORVG_PATCH_APPLIED EQUAL 0)
            execute_process(COMMAND git apply "${THORVG_PATCH}"
                WORKING_DIRECTORY "${ZEPHYR_LVGL_MODULE_DIR}"
                RESULT_VARIABLE THORVG_PATCH_RESULT)
            if(NOT THORVG_PATCH_RESULT EQUAL 0)
                message(FATAL_ERROR "Failed to apply the LVGL ${THORVG_PATCH_NAME} patch")
            endif()
        endif()
    endforeach()

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
