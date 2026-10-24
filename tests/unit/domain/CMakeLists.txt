cmake_minimum_required(VERSION 3.20.0)

set(RT_GAUGE_TEST_SIM_FLASH ON)
set(RT_GAUGE_TEST_LVGL ON)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../rt_gauge_test.cmake)

# Use Zephyr's build system
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(unit_domain)

include_directories(${APP_SRC_DIR})
include_directories(${RT_GAUGE_DIR}/tests/common)

set(APP_SRC_CONFIGURATION_DIR ${APP_SRC_DIR}/configuration)
file(GLOB_RECURSE APP_SRC_CONFIGURATION
  ${APP_SRC_CONFIGURATION_DIR}/*.c
  ${APP_SRC_CONFIGURATION_DIR}/*.cpp
)

set(APP_SRC_UI_DOMAIN_DIR ${APP_SRC_DIR}/domain/ui_domain)
file(GLOB_RECURSE APP_SRC_UI_DOMAIN
  ${APP_SRC_UI_DOMAIN_DIR}/models/*.cpp
  ${APP_SRC_UI_DOMAIN_DIR}/configuration/parsers/*.cpp
  ${APP_SRC_UI_DOMAIN_DIR}/preview/*.cpp
)

file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

# Add all source files to the app target
target_sources(app PRIVATE
  ${APP_SRC_CONFIGURATION}
  ${APP_SRC_UI_DOMAIN}
  ${APP_SRC_DIR}/domain/display_domain/configuration/parsers/display_configuration_cbor_parser.cpp
  ${APP_SRC_DIR}/views/utilitites/grid_layout.cpp
  ${TEST_SOURCES}
)
