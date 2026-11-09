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
#define CO5300_W_HIST_BINS 8

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
	uint32_t write_retries;        /* display_write() re-sends after -EIO / -ETIMEDOUT; they also count in caset, paset, pixel_transfers and pixel_bytes */
	uint32_t write_failures;       /* writes that failed on every attempt */
	uint32_t other_cmds;           /* command transactions outside display_write() (brightness, blanking, ...) */

	/* W-only write time: from after the TE wait to the end of the last pixel transaction, for writes
	 * that succeeded at the first attempt. Full-frame areas and all others are kept apart. */
	uint32_t w_full_count;
	uint64_t w_full_cycles;
	uint32_t w_full_min;           /* valid when w_full_count > 0 */
	uint32_t w_full_max;
	uint32_t w_part_count;
	uint64_t w_part_cycles;
	uint32_t w_part_min;           /* valid when w_part_count > 0 */
	uint32_t w_part_max;
	uint32_t w_resent;             /* writes that needed a re-send; left out of the aggregates above */
	uint32_t w_full_hist[CO5300_W_HIST_BINS]; /* full frames: < 22 ms, 0.5 ms bins up to 25 ms, >= 25 ms */

	/* TE edges (CONFIG_ELCO5300_TE_SYNC); times in k_cycle_get_64() cycles */
	uint32_t te_falls;
	uint32_t te_rises;
	uint32_t te_coalesced;         /* samples dropped because their level equalled the previous one */
	uint64_t te_s_cycles;          /* scan period estimate, 0 until valid */
	bool te_s_valid;
	uint32_t te_iv_count;          /* accepted falling-edge intervals */
	uint64_t te_iv_min;            /* valid when te_iv_count > 0 */
	uint64_t te_iv_max;
	uint32_t te_tvdl_count;        /* fall to rise */
	uint64_t te_tvdl_sum;
	uint64_t te_tvdl_max;
	uint32_t te_tvdh_count;        /* rise to fall */
	uint64_t te_tvdh_sum;
	uint64_t te_tvdh_max;
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

/** @brief One TE edge recorded by the capture facility. */
struct co5300_te_sample {
	uint64_t cycles; /* k_cycle_get_64() */
	uint8_t level;   /* pin level after the edge: 1 = TE high (porch), 0 = low (scanning) */
};

/**
 * @brief Switch the TE GPIO interrupt off or on again (both edges). With it off the TE wait of a
 * frame start times out, which lets frames continue.
 */
int co5300_te_irq_enable(const struct device *dev, bool enable);

/**
 * @brief Select TE mode 1 (line 0: STESL with N = 0) or TE mode 3 (a pulse when the panel reaches the
 * scan line; STESL takes effect on the frame after the current one).
 */
int co5300_te_set_scanline(const struct device *dev, uint16_t line);

/**
 * @brief Arm the capture: the TE interrupt only stores every edge (level and cycles) in buf, until
 * count samples are in. The estimator and the frame start wait see nothing meanwhile.
 *
 * @retval 0 armed, -EBUSY already armed, -ENOTSUP without TE sync, -EINVAL bad arguments.
 */
int co5300_te_capture_start(const struct device *dev, struct co5300_te_sample *buf, size_t count);

/**
 * @brief Wait for the capture to fill and disarm it.
 *
 * @param got Receives the number of samples stored (all of them on success).
 * @retval 0 the capture is full, -ETIMEDOUT it was disarmed with fewer (call it only after a start).
 */
int co5300_te_capture_wait(const struct device *dev, k_timeout_t timeout, size_t *got);
#endif /* CONFIG_ELCO5300_STATS */

#ifdef __cplusplus
}
#endif
