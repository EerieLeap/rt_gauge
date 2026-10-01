cmake_minimum_required(VERSION 3.20.0)

set(RT_GAUGE_TEST_SIM_FLASH ON)

# Deliberately NOT RT_GAUGE_TEST_LVGL: UiController goes through UiRendererService,
# which refuses to start without a chosen display, so this suite binds LVGL to the
# dummy display driver (see boards/native_sim.overlay) instead of running headless.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../rt_gauge_test.cmake)

# Use Zephyr's build system
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(functional_controllers)

include(${RT_GAUGE_DIR}/cmake/lvgl_vector.cmake)

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

# LottieWidget keeps ThorVG on the external heap.
set(APP_SRC_UTILITIES ${APP_SRC_DIR}/utilities/memory/scoped_new_resource.cpp)

# UiController and ManagementController: the others pull in CAN, BLE and the sensor
# stack, none of which this suite exercises.
file(GLOB_RECURSE APP_SRC_CONTROLLERS
  ${APP_SRC_DIR}/controllers/ui_controller.cpp
  ${APP_SRC_DIR}/controllers/management_controller.cpp
)

file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

# Add all source files to the app target
target_sources(app PRIVATE
  ${APP_SRC_CONFIGURATION}
  ${APP_SRC_CONTROLLERS}
  ${APP_SRC_EVENT_BUS}
  ${APP_SRC_UI_DOMAIN}
  ${APP_SRC_UTILITIES}
  ${APP_SRC_VIEWS}
  ${TEST_SOURCES}
)

# The SMP test client injects requests with smp_rx_req().
target_include_directories(app PRIVATE
  ${RT_GAUGE_DIR}/modules/eerie_leap_rt_core/tests/common
  ${ZEPHYR_BASE}/subsys/mgmt/mcumgr/transport/include
)
