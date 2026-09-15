# Zephyr's LVGL module collects C sources only. Build the bundled ThorVG C++
# renderer on the same target so it inherits LVGL's configuration and allocator.
if(CONFIG_LV_USE_VECTOR_GRAPHIC AND CONFIG_LV_USE_THORVG_INTERNAL)
    # Tests do not run app/CMakeLists.txt's patch step. Apply the same stack fix
    # here as well so every target using ThorVG gets it, including a fresh checkout.
    set(THORVG_STACK_PATCH
        "${CMAKE_CURRENT_LIST_DIR}/../app/patches/modules/lvgl/lvgl_thorvg_raster_workspace.patch")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${THORVG_STACK_PATCH}")
    execute_process(COMMAND git apply --reverse --check "${THORVG_STACK_PATCH}"
        WORKING_DIRECTORY "${ZEPHYR_LVGL_MODULE_DIR}"
        RESULT_VARIABLE THORVG_PATCH_APPLIED OUTPUT_QUIET ERROR_QUIET)
    if(NOT THORVG_PATCH_APPLIED EQUAL 0)
        execute_process(COMMAND git apply "${THORVG_STACK_PATCH}"
            WORKING_DIRECTORY "${ZEPHYR_LVGL_MODULE_DIR}"
            RESULT_VARIABLE THORVG_PATCH_RESULT)
        if(NOT THORVG_PATCH_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to apply the ThorVG raster workspace patch")
        endif()
    endif()

    file(GLOB THORVG_SOURCES CONFIGURE_DEPENDS
        "${ZEPHYR_LVGL_MODULE_DIR}/src/libs/thorvg/*.cpp")
    target_sources(modules__lvgl PRIVATE ${THORVG_SOURCES})
    # The bundled SVG loader calls strcasecmp without including its POSIX header.
    # glibc exposes it indirectly; Picolibc requires the declaration explicitly.
    set_source_files_properties("${ZEPHYR_LVGL_MODULE_DIR}/src/libs/thorvg/tvgSvgLoader.cpp"
        TARGET_DIRECTORY modules__lvgl PROPERTIES COMPILE_OPTIONS "-include;strings.h")
endif()
