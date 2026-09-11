# Copyright (c) 2026 Hongquan Li
# SPDX-License-Identifier: Apache-2.0

if(CONFIG_BOARD_ESP32P4_WIFI6_TOUCH_LCD_5_ESP32P4_HPCORE AND CONFIG_PWM)
	zephyr_library()
	zephyr_library_sources(board.c)
endif()
