cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(unit_te_window)

# The window rule is a header of plain C: no rt_core, no LVGL.
# Normalised: a path with ".." keeps /tests/ and pulls the header into clang-tidy's header filter.
get_filename_component(DISPLAY_DRIVER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../drivers/display ABSOLUTE)
target_include_directories(app PRIVATE ${DISPLAY_DRIVER_DIR})

target_sources(app PRIVATE src/te_window_test.c)
