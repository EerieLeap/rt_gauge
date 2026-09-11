/*
 * Copyright (c) 2026 Hongquan Li
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/pwm.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(esp32p4_wifi6_touch_lcd_5, CONFIG_LOG_DEFAULT_LEVEL);

/* The AP3032 LED driver takes its level from the averaged PWM on GPIO26, so the
 * panel stays dark until a duty cycle is programmed. Nothing else in the
 * display chain owns the backlight: neither the DSI bridge nor the HX8394
 * implements display_set_brightness().
 */
static int board_backlight_on(void)
{
	static const struct pwm_dt_spec backlight = PWM_DT_SPEC_GET(DT_ALIAS(pwm_led0));
	int ret;

	if (!pwm_is_ready_dt(&backlight)) {
		LOG_ERR("LCD backlight PWM is not ready");
		return -ENODEV;
	}

	ret = pwm_set_pulse_dt(&backlight, backlight.period);
	if (ret < 0) {
		LOG_ERR("Failed to turn the LCD backlight on (%d)", ret);
	}

	return ret;
}

/* APPLICATION runs after every POST_KERNEL entry, so the panel has already been
 * initialized and the first frame is on its way out.
 */
SYS_INIT(board_backlight_on, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
