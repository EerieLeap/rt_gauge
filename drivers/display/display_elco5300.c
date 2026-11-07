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

	/* Exactly what co5300_set_pixel_format() accepts. */
	capabilities->supported_pixel_formats = PIXEL_FORMAT_L_8 | PIXEL_FORMAT_RGB_565 | PIXEL_FORMAT_RGB_888;
	capabilities->current_pixel_format = data->pixel_format;

	/* Only 0 and 180 degrees exist, so the resolution is never swapped. */
	capabilities->x_resolution = config->width;
	capabilities->y_resolution = config->height;

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

/* One transaction; the caller holds data->lock. */
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
	k_spin_unlock(&data->stats_lock, key);
}

/* One data->lock acquisition by display_write(). */
static void co5300_stats_lock_take(struct co5300_data *data) {
	k_spinlock_key_t key = k_spin_lock(&data->stats_lock);

	data->stats.lock_takes++;
	k_spin_unlock(&data->stats_lock, key);
}
#endif /* CONFIG_ELCO5300_STATS */

static int co5300_lock(struct co5300_data *data) {
	return k_mutex_lock(&data->lock, K_FOREVER);
}

static void co5300_unlock(struct co5300_data *data) {
	k_mutex_unlock(&data->lock);
}

/* The panel forgets nothing on its own, but the cached column range is only valid while the
 * memory-write state it was sent for is: drop it after errors, blanking, reset, orientation and
 * pixel format changes. */
static void co5300_forget_window(struct co5300_data *data) {
	data->caset_valid = false;
}

/* Switches the bus IO mode only when it changes. The caller holds data->lock. */
static int co5300_set_io_mode(const struct device *dev, enum mspi_io_mode io_mode) {
	const struct co5300_config *config = dev->config;
	struct co5300_data *data = dev->data;

	if (data->io_mode_valid && data->mspi_dev_config.io_mode == io_mode) {
		return 0;
	}

	data->mspi_dev_config.io_mode = io_mode;
	int ret = mspi_dev_config(config->mspi_bus, &config->dev_id,
		MSPI_DEVICE_CONFIG_IO_MODE, &data->mspi_dev_config);
	data->io_mode_valid = (ret == 0);

	return ret;
}

/* One transaction. The caller holds data->lock for the whole operation. */
static int co5300_transmit_kind(const struct device *dev, enum mspi_io_mode io_mode, const uint8_t *cmd, const void *tx_data, size_t tx_len, bool hold_ce, enum co5300_stat_kind kind) {
	const struct co5300_config *config = dev->config;
	struct co5300_data *data = dev->data;
	int ret = 0;

#ifdef CONFIG_ELCO5300_STATS
	co5300_stats_transaction(data, kind, tx_len);
#else
	ARG_UNUSED(kind);
#endif

	ret = co5300_set_io_mode(dev, io_mode);
	if (ret < 0) {
		LOG_ERR("Failed to set IO mode: %d", ret);
		return ret;
	}

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
		co5300_forget_window(data);
	}

	return ret;
}

static int co5300_transmit(const struct device *dev, enum mspi_io_mode io_mode, const uint8_t *cmd, const void *tx_data, size_t tx_len, bool hold_ce) {
	return co5300_transmit_kind(dev, io_mode, cmd, tx_data, tx_len, hold_ce, CO5300_STAT_OTHER);
}

static int co5300_reset(const struct device *dev) {
	const struct co5300_config *config = dev->config;

	co5300_forget_window(dev->data);

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
	struct co5300_data *data = dev->data;
	int ret = co5300_lock(data);

	if (ret < 0) {
		return ret;
	}

	LOG_DBG("Turning display blanking off");
	co5300_forget_window(data);

	uint8_t cmd = CO5300_C_SLPOUT;
	ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, NULL, 0, false);
	if (ret == 0) {
		k_sleep(K_MSEC(CO5300_SLPOUT_DELAY));

		cmd = CO5300_C_DISPON;
		ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, NULL, 0, false);
	}
	if (ret == 0) {
		k_sleep(K_MSEC(10));
	}

	co5300_unlock(data);
	return ret;
}

static int co5300_blanking_on(const struct device *dev) {
	struct co5300_data *data = dev->data;
	int ret = co5300_lock(data);

	if (ret < 0) {
		return ret;
	}

	LOG_DBG("Turning display blanking on");
	co5300_forget_window(data);

	uint8_t cmd = CO5300_C_DISPOFF;
	ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, NULL, 0, false);
	if (ret == 0) {
		k_sleep(K_MSEC(CO5300_SLPIN_DELAY));

		cmd = CO5300_C_SLPIN;
		ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, NULL, 0, false);
	}
	if (ret == 0) {
		k_sleep(K_MSEC(CO5300_SLPIN_DELAY));
	}

	co5300_unlock(data);
	return ret;
}

/* The caller holds data->lock. */
static int co5300_set_pixel_format_locked(const struct device *dev, const enum display_pixel_format pixel_format) {
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

	co5300_forget_window(data);

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

static int co5300_set_pixel_format(const struct device *dev, const enum display_pixel_format pixel_format) {
	struct co5300_data *data = dev->data;
	int ret = co5300_lock(data);

	if (ret < 0) {
		return ret;
	}

	ret = co5300_set_pixel_format_locked(dev, pixel_format);
	co5300_unlock(data);

	return ret;
}

/* The caller holds data->lock. */
static int co5300_set_orientation_locked(const struct device *dev, const enum display_orientation orientation) {
	struct co5300_data *data = dev->data;
	uint8_t tx_data = CO5300_MADCTL_RGB;

	switch (orientation) {
	case DISPLAY_ORIENTATION_NORMAL:
		/* No additional flags needed */
		break;
	case DISPLAY_ORIENTATION_ROTATED_180:
		tx_data |= CO5300_MADCTL_MX | CO5300_MADCTL_MY;
		break;
	default:
		/* The controller has no row/column exchange, so 90 and 270 do not exist. */
		LOG_ERR("Unsupported orientation: %d", orientation);
		return -ENOTSUP;
	}

	co5300_forget_window(data);

	uint8_t cmd = CO5300_W_MADCTL;
	int ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, &tx_data, 1U, false);
	if (ret < 0) {
		return ret;
	}

	data->orientation = orientation;

	LOG_DBG("Orientation set to %d", orientation);

	return 0;
}

static int co5300_set_orientation(const struct device *dev, const enum display_orientation orientation) {
	struct co5300_data *data = dev->data;
	int ret = co5300_lock(data);

	if (ret < 0) {
		return ret;
	}

	ret = co5300_set_orientation_locked(dev, orientation);
	co5300_unlock(data);

	return ret;
}

static int co5300_set_brightness(const struct device *dev, const uint8_t brightness) {
	struct co5300_data *data = dev->data;
	int ret = co5300_lock(data);

	if (ret < 0) {
		return ret;
	}

	uint8_t cmd = CO5300_W_WDBRIGHTNESSVALNOR;
	ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, &brightness, 1U, false);
	co5300_unlock(data);

	return ret;
}

/* Sends CASET when the column range changed and PASET always: the datasheet allows a memory write
 * only after CASET, RASET or MADCTL. The caller holds data->lock. */
static int co5300_set_window(const struct device *dev, uint16_t x, uint16_t y, uint16_t width, uint16_t height) {
	const struct co5300_config *config = dev->config;
	struct co5300_data *data = dev->data;

	if (x > config->width || y > config->height) {
		return -EINVAL;
	}

	uint16_t tx_data[2];
	int ret = 0;

	uint16_t x_normalized = x + config->x_offset;
	uint16_t x_start = sys_cpu_to_be16(x_normalized);
	uint16_t x_end = sys_cpu_to_be16(x_normalized + width - 1U);

	if (!data->caset_valid || data->caset_start != x_start || data->caset_end != x_end) {
		tx_data[0] = x_start;
		tx_data[1] = x_end;

		uint8_t cmd = CO5300_W_CASET;
		ret = co5300_transmit_kind(dev, MSPI_IO_MODE_SINGLE, &cmd, tx_data, 4U, false, CO5300_STAT_CASET);
		if (ret < 0) {
			return ret;
		}

		data->caset_start = x_start;
		data->caset_end = x_end;
		data->caset_valid = true;
	}

	uint16_t y_normalized = y + config->y_offset;
	tx_data[0] = sys_cpu_to_be16(y_normalized);
	tx_data[1] = sys_cpu_to_be16(y_normalized + height - 1U);

	uint8_t cmd = CO5300_W_PASET;
	ret = co5300_transmit_kind(dev, MSPI_IO_MODE_SINGLE, &cmd, tx_data, 4U, false, CO5300_STAT_PASET);

	return ret;
}

/* The caller holds data->lock. The quad packet's 0x32 / 0x002C00 header is the memory
 * write command, so there is no separate RAMWR. */
static int co5300_write_locked(const struct device *dev, const uint16_t x, const uint16_t y,
	const struct display_buffer_descriptor *desc, const void *buf) {

	const struct co5300_data *data = dev->data;
	int ret = 0;

	ret = co5300_set_window(dev, x, y, desc->width, desc->height);
	if (ret < 0) {
		return ret;
	}

	/* One quad packet for the whole area; the MSPI driver splits it into hardware transactions. */
	uint8_t cmd = CO5300_C_QUAD_1_1_4;
	size_t total_len = (size_t)desc->height * desc->width * data->bytes_per_pixel;

	ret = co5300_transmit_kind(dev, MSPI_IO_MODE_QUAD_1_1_4, &cmd, buf, total_len, false, CO5300_STAT_PIXEL);
	if (ret < 0) {
		LOG_ERR("Failed to send pixel data: %d", ret);
	}

	return ret;
}

static int co5300_write(const struct device *dev, const uint16_t x, const uint16_t y,
	const struct display_buffer_descriptor *desc, const void *buf) {
	struct co5300_data *data = dev->data;
	int ret;

	if (desc->width == 0U || desc->height == 0U) {
		return -EINVAL;
	}

#ifdef CONFIG_ELCO5300_STATS
	uint32_t start = k_cycle_get_32();

	co5300_stats_write_enter(data, desc, start);
#endif

	ret = co5300_lock(data);
	if (ret < 0) {
		return ret;
	}
#ifdef CONFIG_ELCO5300_STATS
	co5300_stats_lock_take(data);
#endif

	ret = co5300_write_locked(dev, x, y, desc, buf);
	if (ret < 0) {
		co5300_forget_window(data);
	}

	co5300_unlock(data);

#ifdef CONFIG_ELCO5300_STATS
	co5300_stats_write_exit(data, start);
#endif

	return ret;
}

/* The caller holds data->lock. */
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

	ret = co5300_set_pixel_format_locked(dev, config->pixel_format);
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

	ret = co5300_set_orientation_locked(dev, config->orientation);
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

	k_mutex_init(&data->lock);

	ret = mspi_dev_config(config->mspi_bus, &config->dev_id,
		MSPI_DEVICE_CONFIG_IO_MODE |
		MSPI_DEVICE_CONFIG_CPP |
		MSPI_DEVICE_CONFIG_CE_NUM |
		MSPI_DEVICE_CONFIG_CE_POL |
		MSPI_DEVICE_CONFIG_FREQUENCY,
		&data->mspi_dev_config);
	if (ret < 0) {
		LOG_ERR("Failed to configure device: %d", ret);
		return ret;
	}
	data->io_mode_valid = true;
	co5300_forget_window(data);

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

	ret = co5300_lock(data);
	if (ret < 0) {
		return ret;
	}

	ret = co5300_reset(dev);
	if (ret < 0) {
		LOG_ERR("Failed to reset display: %d", ret);
		co5300_unlock(data);
		return ret;
	}

	data->configured = true;

	ret = co5300_configure_display(dev);
	co5300_unlock(data);
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
		.orientation = DT_INST_ENUM_IDX(inst, orientation),             \
		};                                                                  \
	static struct co5300_data co5300_data_##inst = {                    \
		.mspi_dev_config = MSPI_DEVICE_CONFIG_DT_INST(inst),            \
	};                                                                  \
                                                                        \
	DEVICE_DT_INST_DEFINE(inst, &co5300_init, NULL,                     \
		&co5300_data_##inst,                                            \
		&co5300_config_##inst,                                          \
		POST_KERNEL,                                                    \
		CONFIG_DISPLAY_INIT_PRIORITY,                                   \
		&co5300_api);

DT_INST_FOREACH_STATUS_OKAY(CO5300_INIT)
