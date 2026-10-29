cmake_minimum_required(VERSION 3.20.0)

set(RT_GAUGE_TEST_SIM_FLASH ON)
include(${CMAKE_CURRENT_SOURCE_DIR}/../../../rt_gauge_test.cmake)

# Use Zephyr's build system
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(functional_display_domain)

include_directories(${APP_SRC_DIR})

set(APP_SRC_CONFIGURATION_DIR ${APP_SRC_DIR}/configuration)
file(GLOB_RECURSE APP_SRC_CONFIGURATION
  ${APP_SRC_CONFIGURATION_DIR}/*.c
  ${APP_SRC_CONFIGURATION_DIR}/*.cpp
)

# The configuration half of the domain builds everywhere.
set(APP_SRC_DISPLAY_DOMAIN_DIR ${APP_SRC_DIR}/domain/display_domain)
file(GLOB_RECURSE APP_SRC_DISPLAY_DOMAIN
  ${APP_SRC_DISPLAY_DOMAIN_DIR}/configuration/*.c
  ${APP_SRC_DISPLAY_DOMAIN_DIR}/configuration/*.cpp
)

file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

# DisplayService talks to the display driver, which only native_sim binds here (the dummy display of
# boards/native_sim.overlay): the service and its tests are built where the display driver is on.
if(CONFIG_DISPLAY)
  list(APPEND APP_SRC_DISPLAY_DOMAIN
    ${APP_SRC_DISPLAY_DOMAIN_DIR}/services/display_service.cpp
    ${APP_SRC_DIR}/domain/ui_domain/lvgl_lock.cpp
  )
else()
  list(REMOVE_ITEM TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/src/display_service_brightness.cpp)
endif()

# Add all source files to the app target
target_sources(app PRIVATE
  ${APP_SRC_CONFIGURATION}
  ${APP_SRC_DISPLAY_DOMAIN}
  ${TEST_SOURCES}
)
