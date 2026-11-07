/*
 * Copyright (c) 2025 Pavel Maloletkov.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define DT_DRV_COMPAT chipone_elco5300

#include <zephyr/logging/log.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/byteorder.h>

#include "display_elco5300.h"

LOG_MODULE_REGISTER(display_co5300, CONFIG_DISPLAY_LOG_LEVEL);

int co5300_te_gpio_register_callback(const struct device *dev, gpio_callback_handler_t handler) {
	const struct co5300_config *config = dev->config;
	struct co5300_data *data = dev->data;

	int ret = gpio_pin_interrupt_configure_dt(&config->te_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret != 0) {
		LOG_ERR("Error %d: failed to configure interrupt on %s pin %d\n",
			ret, config->te_gpio.port->name, config->te_gpio.pin);
		return ret;
	}

	gpio_init_callback(&data->te_gpio_cb_data, handler, BIT(config->te_gpio.pin));
	gpio_add_callback(config->te_gpio.port, &data->te_gpio_cb_data);

	LOG_INF("Set up Display Tearing Effect Callback at port: %s pin: %d\n", config->te_gpio.port->name, config->te_gpio.pin);

	return 0;
}

static void co5300_get_capabilities(const struct device *dev, struct display_capabilities *capabilities) {
	const struct co5300_data *data = dev->data;
	const struct co5300_config *config = dev->config;

	memset(capabilities, 0, sizeof(struct display_capabilities));

	capabilities->supported_pixel_formats = PIXEL_FORMAT_RGB_565 | PIXEL_FORMAT_RGB_888;
	capabilities->current_pixel_format = data->pixel_format;

	if (data->orientation == DISPLAY_ORIENTATION_NORMAL ||
		data->orientation == DISPLAY_ORIENTATION_ROTATED_180) {
		capabilities->x_resolution = config->width;
		capabilities->y_resolution = config->height;
	} else {
		capabilities->x_resolution = config->height;
		capabilities->y_resolution = config->width;
	}

	capabilities->current_orientation = data->orientation;
}

#ifdef CONFIG_ELCO5300_STATS
void co5300_stats_get(const struct device *dev, struct co5300_stats *out) {
	struct co5300_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->stats_lock);

	*out = data->stats;
	k_spin_unlock(&data->stats_lock, key);
}

void co5300_stats_reset(const struct device *dev) {
	struct co5300_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->stats_lock);

	memset(&data->stats, 0, sizeof(data->stats));
	data->post_pending = false;
	k_spin_unlock(&data->stats_lock, key);
}

void co5300_stats_mark_post(const struct device *dev, uint32_t cycles, bool lifted) {
	struct co5300_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->stats_lock);

	data->post_stamp = cycles;
	data->post_lifted = lifted;
	data->post_pending = true;
	k_spin_unlock(&data->stats_lock, key);
}

/* display_write() entry: counts the write and turns a pending post stamp into one sample. */
static void co5300_stats_write_enter(struct co5300_data *data, const struct display_buffer_descriptor *desc, uint32_t now) {
	k_spinlock_key_t key = k_spin_lock(&data->stats_lock);

	data->stats.writes++;
	if (!desc->frame_incomplete) {
		data->stats.frames++;
	}
	if (data->post_pending) {
		uint32_t delay = now - data->post_stamp;
		uint32_t us = k_cyc_to_us_floor32(delay);

		data->stats.post_samples++;
		data->stats.post_cycles += delay;
		if (delay > data->stats.post_cycles_max) {
			data->stats.post_cycles_max = delay;
		}
		if (data->post_lifted) {
			data->stats.post_lifted++;
			if (delay > data->stats.post_cycles_max_lifted) {
				data->stats.post_cycles_max_lifted = delay;
			}
		}
		data->stats.post_hist[us < 10 ? 0 : us < 100 ? 1 : us < 1000 ? 2 : us < 10000 ? 3 : 4]++;
		data->post_pending = false;
	}
	k_spin_unlock(&data->stats_lock, key);
}

static void co5300_stats_write_exit(struct co5300_data *data, uint32_t start) {
	uint32_t spent = k_cycle_get_32() - start;
	k_spinlock_key_t key = k_spin_lock(&data->stats_lock);

	data->stats.write_cycles += spent;
	if (spent > data->stats.write_cycles_max) {
		data->stats.write_cycles_max = spent;
	}
	k_spin_unlock(&data->stats_lock, key);
}

/* One transaction, counted after the lock is taken. */
static void co5300_stats_transaction(struct co5300_data *data, enum co5300_stat_kind kind, size_t tx_len) {
	k_spinlock_key_t key = k_spin_lock(&data->stats_lock);

	switch (kind) {
	case CO5300_STAT_OTHER:
		data->stats.other_cmds++;
		break;
	case CO5300_STAT_CASET:
		data->stats.caset++;
		break;
	case CO5300_STAT_PASET:
		data->stats.paset++;
		break;
	case CO5300_STAT_RAMWR:
		data->stats.ramwr++;
		break;
	case CO5300_STAT_PIXEL:
		data->stats.pixel_transfers++;
		data->stats.pixel_bytes += tx_len;
		break;
	}
	if (kind != CO5300_STAT_OTHER) {
		data->stats.lock_takes++;
	}
	k_spin_unlock(&data->stats_lock, key);
}
#endif /* CONFIG_ELCO5300_STATS */

static int co5300_transmit_kind(const struct device *dev, enum mspi_io_mode io_mode, const uint8_t *cmd, const void *tx_data, size_t tx_len, bool hold_ce, enum co5300_stat_kind kind) {
	struct co5300_config *config = dev->config;
	struct co5300_data *data = dev->data;
	int ret = 0;

	ret = k_mutex_lock(&data->lock, K_FOREVER);
	if (ret < 0)
		return ret;
#ifdef CONFIG_ELCO5300_STATS
	co5300_stats_transaction(data, kind, tx_len);
#else
	ARG_UNUSED(kind);
#endif

	config->mspi_dev_config.io_mode = io_mode;
	mspi_dev_config(config->mspi_bus, &config->dev_id,
		MSPI_DEVICE_CONFIG_IO_MODE, &config->mspi_dev_config);

	struct mspi_xfer_packet packet = {
		.dir = MSPI_TX,
		.data_buf = (uint8_t *)tx_data,
		.num_bytes = tx_len,
	};

	struct mspi_xfer xfer = {
		.packets = &packet,
		.num_packet = 1,
		.async = false,
		.timeout = SPI_WRITE_TIMEOUT,
		.priority = 0,
		.hold_ce = hold_ce,
		.cmd_length = 1,  // 8-bit command
		.addr_length = 3, // 24-bit address
	};

	if (cmd) {
		uint8_t cmd_data = *cmd;

		if (io_mode == MSPI_IO_MODE_SINGLE) {
			packet.cmd = CO5300_SPI_MODE_SINGLE;
		} else if (io_mode == MSPI_IO_MODE_QUAD_1_1_4) {
			packet.cmd = CO5300_SPI_MODE_QUAD_1_1_4;
		} else {
			LOG_ERR("Unsupported IO mode: %d", io_mode);
			return -ENOTSUP;
		}

		packet.address = (uint32_t)cmd_data << 8;
		xfer.cmd_length = 1;
		xfer.addr_length = 3;
	} else {
		packet.cmd = 0;
		packet.address = 0;
		xfer.cmd_length = 0;
		xfer.addr_length = 0;
	}

	ret = mspi_transceive(config->mspi_bus, &config->dev_id, &xfer);
	if (ret < 0) {
		LOG_ERR("Failed to send qspi packet: %d", ret);
	}

	k_mutex_unlock(&data->lock);
	return ret;
}

static int co5300_transmit(const struct device *dev, enum mspi_io_mode io_mode, const uint8_t *cmd, const void *tx_data, size_t tx_len, bool hold_ce) {
	return co5300_transmit_kind(dev, io_mode, cmd, tx_data, tx_len, hold_ce, CO5300_STAT_OTHER);
}

static int co5300_reset(const struct device *dev) {
	const struct co5300_config *config = dev->config;

	if (config->reset_gpio.port) {
		gpio_pin_set_dt(&config->reset_gpio, 0);
		k_sleep(K_MSEC(CO5300_RST_DELAY));

		gpio_pin_set_dt(&config->reset_gpio, 1);
		k_sleep(K_MSEC(CO5300_RST_DELAY));
	} else {
		LOG_ERR("Failed to reset display, reset gpio is not configured");
		return -ENOTSUP;
	}

	return 0;
}

static int co5300_blanking_off(const struct device *dev) {
	LOG_DBG("Turning display blanking off");

	uint8_t cmd = CO5300_C_SLPOUT;
	int ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, NULL, 0, false);
	if (ret < 0)
		return ret;
	k_sleep(K_MSEC(CO5300_SLPOUT_DELAY));

	cmd = CO5300_C_DISPON;
	ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, NULL, 0, false);
	if (ret < 0)
		return ret;
	k_sleep(K_MSEC(10));

	return 0;
}

static int co5300_blanking_on(const struct device *dev) {
	LOG_DBG("Turning display blanking on");

	uint8_t cmd = CO5300_C_DISPOFF;
	int ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, NULL, 0, false);
	if (ret < 0)
		return ret;
	k_sleep(K_MSEC(CO5300_SLPIN_DELAY));

	cmd = CO5300_C_SLPIN;
	ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, NULL, 0, false);
	if (ret < 0)
		return ret;
	k_sleep(K_MSEC(CO5300_SLPIN_DELAY));

	return 0;
}

static int co5300_set_pixel_format(const struct device *dev, const enum display_pixel_format pixel_format) {
	struct co5300_data *data = dev->data;
	uint8_t tx_data;
	uint8_t bytes_per_pixel;

	if (pixel_format == PIXEL_FORMAT_L_8) {
		bytes_per_pixel = 1U;
		tx_data = CO5300_PIXFMT_8BIT_GRAY256;
	} else if (pixel_format == PIXEL_FORMAT_RGB_565) {
		bytes_per_pixel = 2U;
		tx_data = CO5300_PIXFMT_16BIT_RGB565;
	} else if (pixel_format == PIXEL_FORMAT_RGB_888) {
		bytes_per_pixel = 3U;
		tx_data = CO5300_PIXFMT_24BIT_RGB888;
	} else {
		LOG_ERR("Unsupported pixel format");
		return -ENOTSUP;
	}

	uint8_t cmd = CO5300_W_PIXFMT;
	int ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, &tx_data, 1U, false);
	if (ret < 0) {
		return ret;
	}

	data->pixel_format = pixel_format;
	data->bytes_per_pixel = bytes_per_pixel;

	LOG_DBG("Pixel format set to %d", pixel_format);

	return 0;
}

static int co5300_set_orientation(const struct device *dev, const enum display_orientation orientation) {
	struct co5300_data *data = dev->data;
	uint8_t tx_data = CO5300_MADCTL_RGB;

	switch (orientation) {
	case DISPLAY_ORIENTATION_NORMAL:
		/* No additional flags needed */
		break;
	case DISPLAY_ORIENTATION_ROTATED_90:
		tx_data |= CO5300_MADCTL_X_AXIS_FLIP;
		break;
	case DISPLAY_ORIENTATION_ROTATED_180:
		tx_data |= CO5300_MADCTL_X_AXIS_FLIP | CO5300_MADCTL_Y_AXIS_FLIP;
		break;
	case DISPLAY_ORIENTATION_ROTATED_270:
		tx_data |= CO5300_MADCTL_Y_AXIS_FLIP;
		break;
	default:
		LOG_ERR("Unsupported orientation: %d", orientation);
		return -ENOTSUP;
	}

	uint8_t cmd = CO5300_W_MADCTL;
	int ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, &tx_data, 1U, false);
	if (ret < 0) {
		return ret;
	}

	data->orientation = orientation;

	LOG_DBG("Orientation set to %d", orientation);

	return ret;
}

static int co5300_set_brightness(const struct device *dev, const uint8_t brightness) {
	uint8_t cmd = CO5300_W_WDBRIGHTNESSVALNOR;
	int ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, &brightness, 1U, false);

	return ret;
}

static int co5300_set_window(const struct device *dev, uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
    const struct co5300_config *config = dev->config;

	struct display_capabilities capabilities;
	co5300_get_capabilities(dev, &capabilities);
	if (x > capabilities.x_resolution || y > capabilities.y_resolution) {
		return -EINVAL;
	}

	uint16_t tx_data[2];
	int ret = 0;

    uint16_t x_normalized = x + config->x_offset;
	tx_data[0] = sys_cpu_to_be16(x_normalized);
	tx_data[1] = sys_cpu_to_be16(x_normalized + width - 1U);

	uint8_t cmd = CO5300_W_CASET;
	ret = co5300_transmit_kind(dev, MSPI_IO_MODE_SINGLE, &cmd, tx_data, 4U, false, CO5300_STAT_CASET);
	if (ret < 0) {
		return ret;
	}

    uint16_t y_normalized = y + config->y_offset;
	tx_data[0] = sys_cpu_to_be16(y_normalized);
	tx_data[1] = sys_cpu_to_be16(y_normalized + height - 1U);

	cmd = CO5300_W_PASET;
	ret = co5300_transmit_kind(dev, MSPI_IO_MODE_SINGLE, &cmd, tx_data, 4U, false, CO5300_STAT_PASET);

	return ret;
}

static int co5300_write_inner(const struct device *dev, const uint16_t x, const uint16_t y,
	const struct display_buffer_descriptor *desc, const void *buf) {

	const struct co5300_config *config = dev->config;
	const struct co5300_data *data = dev->data;
	int ret = 0;

	if(desc->width == 0U || desc->height == 0U) {
		return -EINVAL;
	}

	ret = co5300_set_window(dev, x, y, desc->width, desc->height);
	if (ret < 0) {
		return ret;
	}

	uint8_t cmd = CO5300_W_RAMWR;
	ret = co5300_transmit_kind(dev, MSPI_IO_MODE_SINGLE, &cmd, NULL, 0, false, CO5300_STAT_RAMWR);
	if (ret < 0) {
		return ret;
	}

	const uint8_t *tx_data = (const uint8_t *)buf;
	int total_len = desc->height * desc->width * data->bytes_per_pixel;
	int remaining = total_len;
	bool first_send = true;
	bool is_last_send = false;

	while (remaining > 0) {
		int chunk_size = 0;
		if (remaining <= config->max_buf_size) {
			is_last_send = true;
			chunk_size = remaining;
		} else {
			chunk_size = config->max_buf_size;
		}

		cmd = CO5300_C_QUAD_1_1_4;
		ret = co5300_transmit_kind(dev, MSPI_IO_MODE_QUAD_1_1_4, first_send ? &cmd : NULL, tx_data, chunk_size, !is_last_send, CO5300_STAT_PIXEL);
		if (ret < 0) {
			LOG_ERR("Failed to send pixel data chunk: %d", ret);
			return ret;
		}

		tx_data += chunk_size;
		remaining -= chunk_size;
		first_send = false;
	}

	return ret;
}

#ifdef CONFIG_ELCO5300_STATS
static int co5300_write(const struct device *dev, const uint16_t x, const uint16_t y,
	const struct display_buffer_descriptor *desc, const void *buf) {
	struct co5300_data *data = dev->data;
	uint32_t start = k_cycle_get_32();

	co5300_stats_write_enter(data, desc, start);
	int ret = co5300_write_inner(dev, x, y, desc, buf);
	co5300_stats_write_exit(data, start);

	return ret;
}
#else
#define co5300_write co5300_write_inner
#endif

static int co5300_configure_display(const struct device *dev) {
	const struct co5300_config *config = dev->config;
	int ret = 0;

	// Exit sleep mode
	uint8_t cmd = CO5300_C_SLPOUT;
	ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, NULL, 0, false);
	if (ret < 0)
		return ret;
	k_sleep(K_MSEC(CO5300_SLPOUT_DELAY));

	// Tearing effect on
	cmd = CO5300_WC_TEARON;
	ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, NULL, 0, false);
	if (ret < 0)
		return ret;

	// Set SPI mode
	cmd = CO5300_W_SPIMODECTL;
	uint8_t tx_data = 0x80;
	ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, &tx_data, 1, false);
	if (ret < 0)
		return ret;

	ret = co5300_set_pixel_format(dev, config->pixel_format);
	if (ret < 0)
		return ret;

	// Write CTRL Display1
	cmd = CO5300_W_WCTRLD1;
	tx_data = 0x20;
	ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, &tx_data, 1, false);
	if (ret < 0)
		return ret;

	// Write Display Brightness Value in HBM Mode
	cmd = CO5300_W_WDBRIGHTNESSVALHBM;
	tx_data = 0xFF;
	ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, &tx_data, 1, false);
	if (ret < 0)
		return ret;

	// Display on
	cmd = CO5300_C_DISPON;
	ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, NULL, 0, false);
	if (ret < 0)
		return ret;
	k_sleep(K_MSEC(10));

	// Write Display Brightness Value in Normal Mode
	cmd = CO5300_W_WDBRIGHTNESSVALNOR;
	tx_data = 0xFF;
	ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, &tx_data, 1, false);
	if (ret < 0)
		return ret;

	ret = co5300_set_orientation(dev, config->orientation);
	if (ret < 0)
		return ret;

	LOG_DBG("CO5300 display configured successfully");

	return ret;
}

static int co5300_init(const struct device *dev) {
	const struct co5300_config *config = dev->config;
	struct co5300_data *data = dev->data;
	int ret;

	if (!device_is_ready(config->mspi_bus)) {
		LOG_ERR("MSPI device is not ready");
		return -ENODEV;
	}

	ret = mspi_dev_config(config->mspi_bus, &config->dev_id,
		MSPI_DEVICE_CONFIG_IO_MODE |
		MSPI_DEVICE_CONFIG_CPP |
		MSPI_DEVICE_CONFIG_CE_NUM |
		MSPI_DEVICE_CONFIG_CE_POL |
		MSPI_DEVICE_CONFIG_FREQUENCY,
		&config->mspi_dev_config);
	if (ret < 0) {
		LOG_ERR("Failed to configure device: %d", ret);
		return ret;
	}

	k_mutex_init(&data->lock);

	if (config->te_gpio.port) {
		ret = gpio_pin_configure_dt(&config->te_gpio, GPIO_INPUT);
		if (ret < 0) {
			LOG_ERR("Failed to configure TE GPIO (%d)", ret);
			return ret;
		}
	}

	ret = gpio_pin_configure_dt(&config->reset_gpio, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure reset GPIO (%d)", ret);
		return ret;
	}

	ret = co5300_reset(dev);
	if (ret < 0) {
		LOG_ERR("Failed to reset display: %d", ret);
		return ret;
	}

	data->configured = true;

	ret = co5300_configure_display(dev);
	if (ret < 0) {
		LOG_ERR("Failed to configure display: %d", ret);
		return ret;
	}

	LOG_DBG("CO5300 display initialized successfully");
	return 0;
}

static DEVICE_API(display, co5300_api) = {
	.blanking_on = co5300_blanking_on,
	.blanking_off = co5300_blanking_off,
	.write = co5300_write,
	.get_capabilities = co5300_get_capabilities,
	.set_pixel_format = co5300_set_pixel_format,
	.set_orientation = co5300_set_orientation,
	.set_brightness = co5300_set_brightness,
};

#define CO5300_INIT(inst)                                               \
	static const struct co5300_config co5300_config_##inst = {          \
		.mspi_bus = DEVICE_DT_GET(DT_INST_BUS(inst)),                   \
		.dev_id = {                                                     \
			.dev_idx = DT_INST_REG_ADDR(inst),                          \
		},                                                              \
		.te_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, te_gpios, {}),        \
		.reset_gpio = GPIO_DT_SPEC_INST_GET_OR(inst, reset_gpios, {}),  \
		.height = DT_INST_PROP(inst, height),                           \
		.width = DT_INST_PROP(inst, width),                             \
        .x_offset = DT_INST_PROP(inst, x_offset),                       \
		.y_offset = DT_INST_PROP(inst, y_offset),                       \
		.pixel_format = DT_INST_PROP(inst, pixel_format),               \
		.write_only = DT_INST_PROP(inst, write_only),                   \
		.orientation = DT_INST_ENUM_IDX(inst, orientation),             \
		.display_invert = DT_INST_PROP(inst, display_invert),           \
		.color_invert = DT_INST_PROP(inst, color_invert),               \
		.sram_size = DT_INST_PROP(inst, sram_size),          			\
		.max_buf_size = DT_INST_PROP(inst, max_buf_size),				\
		.mspi_dev_config = MSPI_DEVICE_CONFIG_DT_INST(inst),			\
	};                                                                  \
	static struct co5300_data co5300_data_##inst;                       \
                                                                        \
	DEVICE_DT_INST_DEFINE(inst, &co5300_init, NULL,                     \
		&co5300_data_##inst,                                            \
		&co5300_config_##inst,                                          \
		POST_KERNEL,                                                    \
		CONFIG_DISPLAY_INIT_PRIORITY,                                   \
		&co5300_api);

DT_INST_FOREACH_STATUS_OKAY(CO5300_INIT)
