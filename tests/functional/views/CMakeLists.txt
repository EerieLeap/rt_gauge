cmake_minimum_required(VERSION 3.20.0)

set(RT_GAUGE_TEST_SIM_FLASH ON)
set(RT_GAUGE_TEST_LVGL ON)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../rt_gauge_test.cmake)

# Use Zephyr's build system
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(functional_views)

include(${RT_GAUGE_DIR}/cmake/lvgl_vector.cmake)
include(${RT_GAUGE_DIR}/cmake/builtin_fonts.cmake)

include_directories(${APP_SRC_DIR})

set(APP_SRC_CONFIGURATION_DIR ${APP_SRC_DIR}/configuration)
file(GLOB_RECURSE APP_SRC_CONFIGURATION
  ${APP_SRC_CONFIGURATION_DIR}/*.c
  ${APP_SRC_CONFIGURATION_DIR}/*.cpp
)

set(APP_SRC_UI_DOMAIN_DIR ${APP_SRC_DIR}/domain/ui_domain)
file(GLOB_RECURSE APP_SRC_UI_DOMAIN
  ${APP_SRC_UI_DOMAIN_DIR}/*.c
  ${APP_SRC_UI_DOMAIN_DIR}/*.cpp
)

# InitializeEventChannels() registers the channels widget bindings resolve through.
set(APP_SRC_EVENT_BUS_DIR ${APP_SRC_DIR}/event_bus)
file(GLOB_RECURSE APP_SRC_EVENT_BUS
  ${APP_SRC_EVENT_BUS_DIR}/*.c
  ${APP_SRC_EVENT_BUS_DIR}/*.cpp
)

set(APP_SRC_VIEWS_DIR ${APP_SRC_DIR}/views)
file(GLOB_RECURSE APP_SRC_VIEWS
  ${APP_SRC_VIEWS_DIR}/*.c
  ${APP_SRC_VIEWS_DIR}/*.cpp
)

# The bundled images are ~430 KB of pixel data each and nothing under views/
# references them; the fonts are needed by the themes.
list(FILTER APP_SRC_VIEWS EXCLUDE REGEX "/views/assets/images/")

# LottieWidget keeps ThorVG on the external heap.
set(APP_SRC_UTILITIES ${APP_SRC_DIR}/utilities/memory/scoped_new_resource.cpp)

file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

# Add all source files to the app target
target_sources(app PRIVATE
  ${APP_SRC_CONFIGURATION}
  ${APP_SRC_EVENT_BUS}
  ${APP_SRC_UI_DOMAIN}
  ${APP_SRC_UTILITIES}
  ${APP_SRC_VIEWS}
  ${TEST_SOURCES}
)

zephyr_link_libraries(-Wl,--wrap=lv_anim_start)

# A TrueType font small enough for the suite's heap, taken from the LVGL checkout so that no font binary lives here.
generate_inc_file_for_target(app ${ZEPHYR_LVGL_MODULE_DIR}/tests/src/test_assets/test_gpos_one.ttf
  ${ZEPHYR_BINARY_DIR}/include/generated/test_font_ttf.inc)
