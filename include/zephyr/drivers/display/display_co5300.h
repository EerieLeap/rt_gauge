/*
 * Copyright (c) 2025 Pavel Maloletkov.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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
	uint32_t te_fall_dejittered;   /* falling edges whose stamp the frame path pulled back (the frame path's edge) */
	uint32_t te_fall_dejitter_max_us; /* ... the largest pull-back */

#ifdef CONFIG_ELCO5300_TE_WINDOW
	/* The window rule (CONFIG_ELCO5300_TE_WINDOW). Rule decisions count per write attempt. */
	uint32_t te_win_starts_now;    /* started without any wait */
	uint32_t te_win_waits;         /* timed waits */
	uint32_t te_win_edge_waits;    /* waits for an edge */
	uint32_t te_win_edge_timeouts; /* ... that timed out */
	uint32_t te_win_fallbacks;     /* attempts handed to the edge rule */
	uint32_t te_win_forced;        /* starts after 4 decisions without a clear start */
	uint64_t te_win_wait_cycles;   /* time spent in the rule's waits */
	uint32_t te_win_phase_hist[CO5300_W_HIST_BINS]; /* start phase over S, eight bins, every attempt with timing */
	uint32_t te_win_late;          /* first attempts the audit found torn */
	uint32_t te_win_late_resend;   /* re-sent attempts the audit found torn */
	uint32_t te_win_scans_hist[5]; /* scans a full frame stayed up: 1, 2, 3, 4, 5 or more */
	uint32_t te_win_margin_us;
	uint32_t te_win_wmax_us;       /* current write time bounds of the frame class */
	uint32_t te_win_wmin_us;
	bool te_win_on;                /* the runtime switch */

#ifdef CONFIG_ELCO5300_FRAME_WRITE
	/* co5300_write_frame() (CONFIG_ELCO5300_FRAME_WRITE). The per-rect counters above (writes, lock_takes,
	 * write_retries, write_failures, w_*) count a batch's rects too; frames counts a batch once. */
	uint32_t frame_writes;         /* calls */
	uint32_t frame_rects;          /* rects handed in */
	uint32_t frame_rects_max;      /* the largest frame */
	uint32_t frame_searched;       /* the sorted order failed and the order search ran */
	uint32_t frame_search_us_max;  /* the longest planning time of one plan, search included */
	uint32_t frame_no_window;      /* no order and scan held the frame: written rect by rect */
	uint32_t frame_split;          /* a rect missed its window at run time: the rest went to a later scan */
	uint32_t frame_rounds_exhausted; /* a rect used up its wait rounds (also counted in frame_split) */
	uint32_t frame_resend_split;   /* a re-send could not make the target scan */
	uint32_t frame_edge_fallbacks; /* no timing at the start: edge rule */
	uint32_t frame_timing_lost;    /* timing lost in the middle of a frame */
	uint32_t frame_bound_moved;    /* the planned scan equals a positive bound from the previous frame */
	uint32_t frame_paced;          /* rects that slept for their window after the previous rect */
	uint64_t frame_pace_us;        /* ... and the time */
	uint64_t frame_first_wait_us;  /* waits before the first rect */
	uint32_t frame_gap_max_us;     /* the longest rect end to next start without a pause */
	uint64_t frame_us;             /* first rect's start to last rect's end, summed */
	uint32_t frame_us_max;
	uint32_t frame_mixed;          /* the audit: some scan showed part of the frame new and part old */
	uint32_t frame_late;           /* ... some rect took longer than its bound plus the margin */
	uint32_t frame_out_of_order;   /* ... first shown before the previous frame's bound */
	uint32_t frame_failed_rects;   /* rects the panel did not get */
	uint32_t frame_aborted;        /* frames cut short by -ETIMEDOUT on every attempt */
	uint32_t frame_target_hist[4]; /* target scan from the plan's edge: 0, 1, 2, 3 or more */
	uint32_t frame_scans_hist[5];  /* scans a frame stayed up: 0 (replaced in the same scan), 1, 2, 3, 4 or more */
#endif
#endif
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

#ifdef CONFIG_ELCO5300_TE_WINDOW
/**
 * @brief Switch the window rule on or off at run time (it is on after boot). Off means the edge rule.
 *
 * @retval 0 done, -ENOTSUP without TE sync.
 */
int co5300_te_window_enable(const struct device *dev, bool enable);
#endif
#endif /* CONFIG_ELCO5300_STATS */

#ifdef CONFIG_ELCO5300_FRAME_WRITE
/** One rectangle: w * h pixels in the panel's format, rows back to back (pitch == w). The buffer is 4-byte aligned;
 *  a PSRAM buffer goes out by DMA without a copy only when it starts on a cache line (64 B). */
struct co5300_rect {
	const void *buf;
	uint16_t x;
	uint16_t y;
	uint16_t w;
	uint16_t h;
};

/** Driver scratch for one frame, in plain arrays so the caller can place it anywhere (PSRAM is fine). The
 *  per-rect arrays are indexed by the rect's index in rects[]; the others by the position in the current plan. */
struct co5300_frame_work {
	uint8_t order[CONFIG_ELCO5300_FRAME_MAX_RECTS];
	uint8_t best[CONFIG_ELCO5300_FRAME_MAX_RECTS];       /* the order search's best so far; a spare between plans */
	uint8_t stack[6];                                    /* search levels (CO5300_TE_FRAME_SEARCH_MAX, a BUILD_ASSERT checks it) */
	int64_t stack_t_us[6];                               /* time reached at each level */
	uint8_t todo[CONFIG_ELCO5300_FRAME_MAX_RECTS];       /* rects not written yet, the plan's input */
	uint16_t sy0[CONFIG_ELCO5300_FRAME_MAX_RECTS];       /* the plan's rects: first and last row, write time bounds */
	uint16_t sy1[CONFIG_ELCO5300_FRAME_MAX_RECTS];
	uint32_t sw_min_us[CONFIG_ELCO5300_FRAME_MAX_RECTS];
	uint32_t sw_max_us[CONFIG_ELCO5300_FRAME_MAX_RECTS];
	uint16_t y0[CONFIG_ELCO5300_FRAME_MAX_RECTS];
	uint16_t y1[CONFIG_ELCO5300_FRAME_MAX_RECTS];
	uint32_t w_min_us[CONFIG_ELCO5300_FRAME_MAX_RECTS];  /* frozen at plan time */
	uint32_t w_max_us[CONFIG_ELCO5300_FRAME_MAX_RECTS];
	int64_t a_us[CONFIG_ELCO5300_FRAME_MAX_RECTS];       /* the plan's forbidden start intervals */
	int64_t b_us[CONFIG_ELCO5300_FRAME_MAX_RECTS];
	int64_t start_us[CONFIG_ELCO5300_FRAME_MAX_RECTS];   /* measured, driver us clock */
	int64_t end_us[CONFIG_ELCO5300_FRAME_MAX_RECTS];
};

struct co5300_frame {
	uint8_t n;                                             /* in: rectangles in rects[] */
	struct co5300_rect rects[CONFIG_ELCO5300_FRAME_MAX_RECTS];
	uint8_t failed;                                        /* out: rectangles the panel did not get */
	uint8_t failed_index[CONFIG_ELCO5300_FRAME_MAX_RECTS]; /* out: their indices in rects[] */
	struct co5300_frame_work work;                         /* driver scratch */
};

/**
 * @brief Write every rectangle of one frame so that no panel scan shows some of them new and others old.
 *
 * With valid TE timing (normal orientation, not blanked, window rule on) the driver picks one target scan, never
 * earlier than the bound the previous frame left, and starts each rectangle inside its own tear-free window for it,
 * sleeping between rectangles where a window opens later. Without timing it waits for the next scan start and writes
 * the rectangles top to bottom (the edge rule). A frame that no order and scan can hold is written rectangle by
 * rectangle, each in its own window, and counted. The driver lock is taken per rectangle.
 *
 * The buffers must stay valid and unchanged until the call returns. Thread context; one caller at a time.
 *
 * @retval 0 Every rectangle was written.
 * @retval -EIO, -ETIMEDOUT Some rectangles failed every attempt or were not sent after a timeout (failed,
 *         failed_index[]); the others were written. Another error of the bus is returned the same way.
 * @retval -EINVAL n is 0 or above CONFIG_ELCO5300_FRAME_MAX_RECTS, or a rectangle is empty or outside the panel.
 */
int co5300_write_frame(const struct device *dev, struct co5300_frame *frame);
#endif

#ifdef __cplusplus
}
#endif
