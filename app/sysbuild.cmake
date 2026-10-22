# Sysbuild extensions of the gauge, included after every image is added (MCUboot, the application)
# and before any of them is configured. Settings: sysbuild.conf. Plan:
# dev_docs/plans/todo/FIRMWARE_UPDATE_PLAN.md.

if(NOT SB_CONFIG_BOOTLOADER_MCUBOOT)
  return()
endif()

# The board's flash partition table, the one the application includes from its overlay
# (app/boards/<board>.overlay), also goes to MCUboot, which is built from the board devicetree and
# its own overlays only. MCUboot and the application then agree on where the slots are.
#
# This is an extra overlay, added to whatever MCUboot already uses. Do not add an
# app/sysbuild/mcuboot/ directory (sysbuild would take it as MCUboot's whole configuration
# directory and want its own prj.conf there), and no app/sysbuild/mcuboot.overlay (it would replace
# the overlays MCUboot finds for the board).
string(REGEX REPLACE "^/" "" board_qualifiers "${BOARD_QUALIFIERS}")
string(REPLACE "/" "_" board_target "${BOARD}_${board_qualifiers}")
set(partitions_dtsi ${APP_DIR}/boards/partitions/${board_target}.dtsi)

if(EXISTS ${partitions_dtsi})
  message(STATUS "MCUboot partition table: ${partitions_dtsi}")
  if(NOT partitions_dtsi IN_LIST mcuboot_EXTRA_DTC_OVERLAY_FILE)
    list(APPEND mcuboot_EXTRA_DTC_OVERLAY_FILE ${partitions_dtsi})
  endif()
  set(mcuboot_EXTRA_DTC_OVERLAY_FILE ${mcuboot_EXTRA_DTC_OVERLAY_FILE}
      CACHE INTERNAL "Application extra DTC overlay file" FORCE
  )
else()
  message(WARNING "No partition table for ${board_target} in ${APP_DIR}/boards/partitions: "
                  "MCUboot builds with the board's own table, which the application may not match.")
endif()

# Size checks (plan 1.4), run at the end of every sysbuild build: MCUboot has to fit its boot
# partition, and the signed application its slot minus the swap trailer. Nothing else checks the
# MCUboot size; the linker does not know the partition. Run it by hand on any build directory:
#   python3 scripts/check_image_sizes.py <build dir>
cmake_path(SET gauge_root NORMALIZE "${APP_DIR}/..")
add_custom_target(check_image_sizes ALL
  COMMAND ${PYTHON_EXECUTABLE} ${gauge_root}/scripts/check_image_sizes.py
          --mcuboot ${CMAKE_BINARY_DIR}/mcuboot --app ${CMAKE_BINARY_DIR}/${DEFAULT_IMAGE}
  COMMENT "Checking the image sizes against the partitions"
  USES_TERMINAL
)
add_dependencies(check_image_sizes mcuboot ${DEFAULT_IMAGE})
