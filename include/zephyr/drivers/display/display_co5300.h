/*
 * Copyright (c) 2025 Pavel Maloletkov.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/display.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register callback for Tearing Effect (TE) GPIO.
 *
 * @param dev Pointer to the device structure for the driver instance.
 * @param handler Callback handler function pointer.
 *
 * @retval 0 If successful.
 */
int co5300_te_gpio_register_callback(const struct device *dev, gpio_callback_handler_t handler);

#ifdef CONFIG_ELCO5300_STATS
/** @brief Bench counters of one CO5300 instance (CONFIG_ELCO5300_STATS). */
struct co5300_stats {
	uint32_t writes;               /* display_write() calls */
	uint32_t frames;               /* writes with desc->frame_incomplete == false */
	uint32_t lock_takes;           /* data->lock acquisitions made inside display_write() (one per write) */
	uint32_t caset;                /* CASET command transactions */
	uint32_t paset;                /* PASET (RASET) command transactions */
	uint32_t ramwr;                /* always 0: the first pixel transaction carries the memory write command */
	uint32_t pixel_transfers;      /* pixel mspi_transceive() calls */
	uint64_t pixel_bytes;
	uint64_t write_cycles;         /* k_cycle_get_32() cycles spent inside display_write(), summed */
	uint32_t write_cycles_max;
	uint32_t post_samples;         /* writes that followed a co5300_stats_mark_post() */
	uint32_t post_lifted;          /* ... whose post was marked lifted */
	uint64_t post_cycles;          /* post-to-entry delay, summed */
	uint32_t post_cycles_max;
	uint32_t post_cycles_max_lifted;
	uint32_t post_hist[5];         /* delay < 10 us, < 100 us, < 1 ms, < 10 ms, >= 10 ms */
	uint32_t te_waits;             /* frame starts that waited for the TE edge (CONFIG_ELCO5300_TE_SYNC) */
	uint32_t te_timeouts;          /* ... that gave up after the timeout */
	uint64_t te_wait_cycles;       /* time spent waiting, summed */
	uint32_t other_cmds;           /* command transactions outside display_write() (brightness, blanking, ...) */
};

/** @brief Consistent snapshot of the counters. */
void co5300_stats_get(const struct device *dev, struct co5300_stats *out);

/** @brief Zero the counters (and drop a pending post stamp). */
void co5300_stats_reset(const struct device *dev);

/**
 * @brief Mark a flush post (LVGL's LV_EVENT_FLUSH_START, on the renderer thread) right before the
 * area goes to the flush thread. The next display_write() entry turns it into one post-to-start sample.
 *
 * @param cycles k_cycle_get_32() at the post.
 * @param lifted True when the post was marked lifted.
 */
void co5300_stats_mark_post(const struct device *dev, uint32_t cycles, bool lifted);
#endif /* CONFIG_ELCO5300_STATS */

#ifdef __cplusplus
}
#endif
