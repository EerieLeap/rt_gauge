cmake_minimum_required(VERSION 3.20.0)

include(${CMAKE_CURRENT_SOURCE_DIR}/../../rt_gauge_test.cmake)

# Use Zephyr's build system
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(functional_event_bus)

# The registry comes from rt_core (CONFIG_EERIE_LEAP_EVENT_BUS); the app sources only supply the
# settings channel header.
include_directories(${APP_SRC_DIR})

file(GLOB_RECURSE TEST_SOURCES src/*.c src/*.cpp)

# Add all source files to the app target
target_sources(app PRIVATE
  ${TEST_SOURCES}
)
