/*
 * Copyright (c) 2026 Hongquan Li
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT waveshare_hx8394

#include <zephyr/drivers/display.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/mipi_dsi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(hx8394_lcd5, CONFIG_DISPLAY_LOG_LEVEL);

#define HX8394_CMD_SLPOUT     0x11
#define HX8394_CMD_DISPOFF    0x28
#define HX8394_CMD_DISPON     0x29
#define HX8394_CMD_MADCTL     0x36
#define HX8394_CMD_COLMOD     0x3A
#define HX8394_CMD_SETMIPI    0xBA

#define HX8394_MADCTL_BGR     BIT(3)
#define HX8394_MADCTL_FLIP_X  BIT(1)
#define HX8394_MADCTL_FLIP_Y  BIT(0)

#define HX8394_SETMIPI_1_LANE 0x60
#define HX8394_SETMIPI_2_LANE 0x61
#define HX8394_SETMIPI_3_LANE 0x62
#define HX8394_SETMIPI_4_LANE 0x63

struct hx8394_lcd5_config {
	const struct device *mipi_dsi;
	const struct gpio_dt_spec reset_gpio;
	struct mipi_dsi_timings timings;
	uint8_t num_of_lanes;
	uint8_t pixel_format;
	uint16_t panel_width;
	uint16_t panel_height;
	uint8_t channel;
};

struct hx8394_lcd5_cmd {
	const uint8_t *data;
	uint8_t len;
	uint16_t delay_ms;
};

#define HX8394_CMD(delay, ...)                                                                     \
	{                                                                                          \
		.data = (const uint8_t[]){__VA_ARGS__},                                            \
		.len = sizeof((const uint8_t[]){__VA_ARGS__}),                                     \
		.delay_ms = (delay),                                                               \
	}

/* Panel-specific bring-up for the Waveshare 5" module, transcribed from the
 * vendor's esp_lcd_hx8394 driver. The power rails, VCOM, GIP timing and gamma
 * here are properties of this glass, not of the HX8394 itself, which is why the
 * in-tree himax,hx8394 values (a Rocktech panel) leave this one dark.
 */
static const struct hx8394_lcd5_cmd hx8394_lcd5_init_cmds[] = {
	HX8394_CMD(0, 0xB9, 0xFF, 0x83, 0x94),
	HX8394_CMD(0, 0xB1, 0x48, 0x0A, 0x6A, 0x09, 0x33, 0x54, 0x71, 0x71, 0x2E, 0x45),
	HX8394_CMD(0, 0xBA, 0x61, 0x03, 0x68, 0x6B, 0xB2, 0xC0),
	HX8394_CMD(0, 0xB2, 0x00, 0x80, 0x64, 0x0C, 0x06, 0x2F),
	HX8394_CMD(0, 0xB4, 0x1C, 0x78, 0x1C, 0x78, 0x1C, 0x78, 0x01, 0x0C, 0x86, 0x75, 0x00,
		   0x3F, 0x1C, 0x78, 0x1C, 0x78, 0x1C, 0x78, 0x01, 0x0C, 0x86),
	HX8394_CMD(0, 0xD3, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x08, 0x32, 0x10, 0x05,
		   0x00, 0x05, 0x32, 0x13, 0xC1, 0x00, 0x01, 0x32, 0x10, 0x08, 0x00, 0x00, 0x37,
		   0x03, 0x07, 0x07, 0x37, 0x05, 0x05, 0x37, 0x0C, 0x40),
	HX8394_CMD(0, 0xD5, 0x18, 0x18, 0x18, 0x18, 0x22, 0x23, 0x20, 0x21, 0x04, 0x05, 0x06,
		   0x07, 0x00, 0x01, 0x02, 0x03, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
		   0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
		   0x18, 0x18, 0x18, 0x19, 0x19, 0x19, 0x19),
	HX8394_CMD(0, 0xD6, 0x18, 0x18, 0x19, 0x19, 0x21, 0x20, 0x23, 0x22, 0x03, 0x02, 0x01,
		   0x00, 0x07, 0x06, 0x05, 0x04, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
		   0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18,
		   0x18, 0x18, 0x18, 0x19, 0x19, 0x18, 0x18),
	HX8394_CMD(0, 0xE0, 0x07, 0x08, 0x09, 0x0D, 0x10, 0x14, 0x16, 0x13, 0x24, 0x36, 0x48,
		   0x4A, 0x58, 0x6F, 0x76, 0x80, 0x97, 0xA5, 0xA8, 0xB5, 0xC6, 0x62, 0x63, 0x68,
		   0x6F, 0x72, 0x78, 0x7F, 0x7F, 0x00, 0x02, 0x08, 0x0D, 0x0C, 0x0E, 0x0F, 0x10,
		   0x24, 0x36, 0x48, 0x4A, 0x58, 0x6F, 0x78, 0x82, 0x99, 0xA4, 0xA0, 0xB1, 0xC0,
		   0x5E, 0x5E, 0x64, 0x6B, 0x6C, 0x73, 0x7F, 0x7F),
	HX8394_CMD(0, 0xCC, 0x0B),
	HX8394_CMD(0, 0xC0, 0x1F, 0x73),
	HX8394_CMD(0, 0xB6, 0x6B, 0x6B),
	HX8394_CMD(0, 0xD4, 0x02),
	HX8394_CMD(0, 0xBD, 0x01),
	HX8394_CMD(0, 0xB1, 0x00),
	HX8394_CMD(0, 0xBD, 0x00),
	HX8394_CMD(0, 0xBF, 0x40, 0x81, 0x50, 0x00, 0x1A, 0xFC, 0x01),
	HX8394_CMD(0, 0x3A, 0x50),
	HX8394_CMD(200, HX8394_CMD_SLPOUT),
	HX8394_CMD(0, 0xB2, 0x00, 0x80, 0x64, 0x0C, 0x06, 0x2F, 0x00, 0x00, 0x00, 0x00, 0xC0,
		   0x18),
	HX8394_CMD(80, HX8394_CMD_DISPON),
};

static int hx8394_lcd5_tx(const struct device *dev, const uint8_t *buf, size_t len)
{
	const struct hx8394_lcd5_config *config = dev->config;
	struct mipi_dsi_msg msg = {
		.cmd = buf[0],
		.tx_buf = &buf[1],
		.tx_len = len - 1,
		.flags = MIPI_DSI_MSG_USE_LPM,
	};
	ssize_t ret;

	switch (len) {
	case 1U:
		msg.type = MIPI_DSI_DCS_SHORT_WRITE;
		break;
	case 2U:
		msg.type = MIPI_DSI_DCS_SHORT_WRITE_PARAM;
		break;
	default:
		msg.type = MIPI_DSI_DCS_LONG_WRITE;
		break;
	}

	ret = mipi_dsi_transfer(config->mipi_dsi, config->channel, &msg);
	if (ret < 0) {
		LOG_ERR("Command %02x failed (%d)", buf[0], (int)ret);
		return -EIO;
	}

	return 0;
}

static int hx8394_lcd5_dcs(const struct device *dev, uint8_t cmd, uint8_t param)
{
	const uint8_t buf[] = {cmd, param};

	return hx8394_lcd5_tx(dev, buf, sizeof(buf));
}

static int hx8394_lcd5_blanking_on(const struct device *dev)
{
	const uint8_t cmd = HX8394_CMD_DISPOFF;

	return hx8394_lcd5_tx(dev, &cmd, sizeof(cmd));
}

static int hx8394_lcd5_blanking_off(const struct device *dev)
{
	const uint8_t cmd = HX8394_CMD_DISPON;

	return hx8394_lcd5_tx(dev, &cmd, sizeof(cmd));
}

/* The DSI bridge owns the framebuffer and is the device zephyr,display points
 * at, so pixels never arrive here.
 */
static int hx8394_lcd5_write(const struct device *dev, const uint16_t x, const uint16_t y,
			     const struct display_buffer_descriptor *desc, const void *buf)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(x);
	ARG_UNUSED(y);
	ARG_UNUSED(desc);
	ARG_UNUSED(buf);

	return -ENOTSUP;
}

static int hx8394_lcd5_set_orientation(const struct device *dev,
				       const enum display_orientation orientation)
{
	uint8_t madctl;

	switch (orientation) {
	case DISPLAY_ORIENTATION_NORMAL:
		madctl = 0;
		break;
	case DISPLAY_ORIENTATION_ROTATED_90:
		madctl = HX8394_MADCTL_FLIP_Y;
		break;
	case DISPLAY_ORIENTATION_ROTATED_180:
		madctl = HX8394_MADCTL_FLIP_X | HX8394_MADCTL_FLIP_Y;
		break;
	case DISPLAY_ORIENTATION_ROTATED_270:
		madctl = HX8394_MADCTL_FLIP_X;
		break;
	default:
		return -ENOTSUP;
	}

	return hx8394_lcd5_dcs(dev, HX8394_CMD_MADCTL, madctl);
}

static void hx8394_lcd5_get_capabilities(const struct device *dev,
					 struct display_capabilities *capabilities)
{
	const struct hx8394_lcd5_config *config = dev->config;

	memset(capabilities, 0, sizeof(struct display_capabilities));
	capabilities->x_resolution = config->panel_width;
	capabilities->y_resolution = config->panel_height;
	capabilities->supported_pixel_formats = config->pixel_format;
	capabilities->current_pixel_format = config->pixel_format;
	capabilities->current_orientation = DISPLAY_ORIENTATION_NORMAL;
}

static DEVICE_API(display, hx8394_lcd5_api) = {
	.blanking_on = hx8394_lcd5_blanking_on,
	.blanking_off = hx8394_lcd5_blanking_off,
	.write = hx8394_lcd5_write,
	.get_capabilities = hx8394_lcd5_get_capabilities,
	.set_orientation = hx8394_lcd5_set_orientation,
};

static int hx8394_lcd5_init(const struct device *dev)
{
	const struct hx8394_lcd5_config *config = dev->config;
	struct mipi_dsi_device mdev = {
		.data_lanes = config->num_of_lanes,
		.pixfmt = config->pixel_format,
		.timings = config->timings,
		/* LPM keeps the host sending this sequence in low power mode,
		 * which is the only mode the panel accepts it in.
		 */
		.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_LPM,
	};
	uint8_t colmod;
	uint8_t lanes;
	int ret;

	switch (config->pixel_format) {
	case MIPI_DSI_PIXFMT_RGB565:
		colmod = 0x55;
		break;
	case MIPI_DSI_PIXFMT_RGB666:
	case MIPI_DSI_PIXFMT_RGB666_PACKED:
		colmod = 0x66;
		break;
	case MIPI_DSI_PIXFMT_RGB888:
		colmod = 0x77;
		break;
	default:
		LOG_ERR("Unsupported pixel format %u", config->pixel_format);
		return -ENOTSUP;
	}

	switch (config->num_of_lanes) {
	case 1:
		lanes = HX8394_SETMIPI_1_LANE;
		break;
	case 2:
		lanes = HX8394_SETMIPI_2_LANE;
		break;
	case 3:
		lanes = HX8394_SETMIPI_3_LANE;
		break;
	case 4:
		lanes = HX8394_SETMIPI_4_LANE;
		break;
	default:
		LOG_ERR("Unsupported lane count %u", config->num_of_lanes);
		return -EINVAL;
	}

	ret = mipi_dsi_attach(config->mipi_dsi, config->channel, &mdev);
	if (ret < 0) {
		LOG_ERR("Could not attach to the MIPI DSI host (%d)", ret);
		return ret;
	}

	if (config->reset_gpio.port != NULL) {
		if (!gpio_is_ready_dt(&config->reset_gpio)) {
			LOG_ERR("Reset GPIO is not ready");
			return -ENODEV;
		}

		ret = gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_ACTIVE);
		if (ret < 0) {
			return ret;
		}

		k_sleep(K_MSEC(10));
		gpio_pin_set_dt(&config->reset_gpio, 0);
		k_sleep(K_MSEC(10));
	}

	ret = hx8394_lcd5_tx(dev, (const uint8_t[]){HX8394_CMD_SLPOUT}, 1);
	if (ret < 0) {
		return ret;
	}
	k_sleep(K_MSEC(120));

	ret = hx8394_lcd5_dcs(dev, HX8394_CMD_MADCTL, 0);
	if (ret < 0) {
		return ret;
	}

	ret = hx8394_lcd5_dcs(dev, HX8394_CMD_COLMOD, colmod);
	if (ret < 0) {
		return ret;
	}

	ret = hx8394_lcd5_dcs(dev, HX8394_CMD_SETMIPI, lanes);
	if (ret < 0) {
		return ret;
	}

	for (size_t i = 0; i < ARRAY_SIZE(hx8394_lcd5_init_cmds); i++) {
		ret = hx8394_lcd5_tx(dev, hx8394_lcd5_init_cmds[i].data,
				     hx8394_lcd5_init_cmds[i].len);
		if (ret < 0) {
			return ret;
		}

		if (hx8394_lcd5_init_cmds[i].delay_ms != 0) {
			k_sleep(K_MSEC(hx8394_lcd5_init_cmds[i].delay_ms));
		}
	}

	LOG_INF("HX8394 panel initialized: %ux%u, %u lanes", config->panel_width,
		config->panel_height, config->num_of_lanes);

	return 0;
}

#define HX8394_LCD5_TIMING_NODE(id) DT_INST_CHILD(id, display_timings)

#define HX8394_LCD5_PANEL(id)                                                                      \
	static const struct hx8394_lcd5_config hx8394_lcd5_config_##id = {                         \
		.mipi_dsi = DEVICE_DT_GET(DT_INST_BUS(id)),                                        \
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(id, reset_gpios, {0}),                      \
		.timings = {                                                                       \
			.hactive = DT_INST_PROP(id, width),                                        \
			.hfp = DT_PROP(HX8394_LCD5_TIMING_NODE(id), hfront_porch),                 \
			.hbp = DT_PROP(HX8394_LCD5_TIMING_NODE(id), hback_porch),                  \
			.hsync = DT_PROP(HX8394_LCD5_TIMING_NODE(id), hsync_len),                  \
			.vactive = DT_INST_PROP(id, height),                                       \
			.vfp = DT_PROP(HX8394_LCD5_TIMING_NODE(id), vfront_porch),                 \
			.vbp = DT_PROP(HX8394_LCD5_TIMING_NODE(id), vback_porch),                  \
			.vsync = DT_PROP(HX8394_LCD5_TIMING_NODE(id), vsync_len),                  \
		},                                                                                 \
		.num_of_lanes = DT_INST_PROP_BY_IDX(id, data_lanes, 0),                            \
		.pixel_format = DT_INST_PROP(id, pixel_format),                                    \
		.panel_width = DT_INST_PROP(id, width),                                            \
		.panel_height = DT_INST_PROP(id, height),                                          \
		.channel = DT_INST_REG_ADDR(id),                                                   \
	};                                                                                         \
	DEVICE_DT_INST_DEFINE(id, &hx8394_lcd5_init, NULL, NULL, &hx8394_lcd5_config_##id,         \
			      POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, &hx8394_lcd5_api);

DT_INST_FOREACH_STATUS_OKAY(HX8394_LCD5_PANEL)
