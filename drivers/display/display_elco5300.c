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

	int ret = 0;

#ifdef CONFIG_ELCO5300_TE_SYNC
	/* With TE sync the pin already runs on both edges; the user handler shares it and sees both. */
	if (!data->te_sync)
#endif
	{
		ret = gpio_pin_interrupt_configure_dt(&config->te_gpio, GPIO_INT_EDGE_TO_ACTIVE);
	}
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

#ifdef CONFIG_ELCO5300_TE_SYNC
/* Feeds an interval estimator with one interval, called under te.lock. An interval counts only
 * within CO5300_TE_ACCEPT_PCT of the estimate; the estimate is valid after CO5300_TE_SEED_COUNT
 * agreeing intervals in a row, and dropped after CO5300_TE_REJECT_LIMIT rejected ones in a row.
 * Rejected intervals never touch it. Returns true when the interval was accepted. */
static bool co5300_est_feed(struct co5300_est *est, uint64_t interval) {
	uint64_t tol = est->cycles / 100 * CO5300_TE_ACCEPT_PCT;
	bool agrees = est->cycles != 0 && interval + tol >= est->cycles && interval <= est->cycles + tol;

	if (!est->valid && !agrees) {
		/* Seeding: start over from this interval. */
		est->cycles = interval;
		est->agree = 1;
		est->rejects = 0;
		return false;
	}

	if (!agrees) {
		if (++est->rejects >= CO5300_TE_REJECT_LIMIT) {
			est->valid = false;
			est->cycles = interval;
			est->agree = 1;
			est->rejects = 0;
		}
		return false;
	}

	est->rejects = 0;
	if (interval >= est->cycles) {
		est->cycles += (interval - est->cycles) / 16;
	} else {
		est->cycles -= (est->cycles - interval) / 16;
	}
	if (!est->valid && ++est->agree >= CO5300_TE_SEED_COUNT) {
		est->valid = true;
	}
	return true;
}

#if defined(CONFIG_ELCO5300_STATS) || defined(CONFIG_ELCO5300_TE_WINDOW)
/* Forgets the previous sample, after the interrupt was off or the capture ran: the next edge
 * has no neighbour to be compared with. Called under te.lock. */
static void co5300_te_forget_edges(struct co5300_te *te) {
	te->have_level = false;
	te->have_fall = false;
	te->have_rise = false;
}
#endif

#ifdef CONFIG_ELCO5300_TE_WINDOW

/* Forgets the edges and the estimates (blanking: the scan stops and starts again), so the
 * window rule restarts from fresh edges. Called under te.lock. */
static void co5300_te_reset(struct co5300_te *te) {
	co5300_te_forget_edges(te);
	memset(&te->s, 0, sizeof(te->s));
	memset(&te->tvdl, 0, sizeof(te->tvdl));
}
#endif

/* ISR context: a TE edge. Low: the panel starts a scan; high: the porch starts. */
static void co5300_te_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins) {
	struct co5300_data *data = CONTAINER_OF(cb, struct co5300_data, te_sync_cb_data);
	const struct co5300_config *config = data->dev->config;
	struct co5300_te *te = &data->te;

	ARG_UNUSED(port);
	ARG_UNUSED(pins);

	/* Read the level first: it is the only way to tell the edges apart. A pulse shorter than the
	 * interrupt latency reads as the previous level and is dropped below. */
	bool level = gpio_pin_get_dt(&config->te_gpio) > 0;
	uint64_t now = k_cycle_get_64();
	bool give = false;
	k_spinlock_key_t key = k_spin_lock(&te->lock);

#ifdef CONFIG_ELCO5300_STATS
	if (te->cap_buf != NULL) {
		te->cap_buf[te->cap_n].cycles = now;
		te->cap_buf[te->cap_n].level = level;
		if (++te->cap_n >= te->cap_count) {
			te->cap_buf = NULL;
			/* The capture saw no edge in the normal state: the next one has no valid neighbour. */
			te->have_level = false;
			te->have_fall = false;
			te->have_rise = false;
			k_sem_give(&te->cap_sem);
		}
		k_spin_unlock(&te->lock, key);
		return;
	}
#endif

	if (te->have_level && te->level == level) {
		/* Interrupts masked during flash or cache operations merge edges. */
#ifdef CONFIG_ELCO5300_STATS
		te->coalesced++;
#endif
		k_spin_unlock(&te->lock, key);
		return;
	}
	te->have_level = true;
	te->level = level;

	if (!level) {
		if (te->have_fall) {
			uint64_t interval = now - te->fall_cycles;
			bool accepted = co5300_est_feed(&te->s, interval);
#ifdef CONFIG_ELCO5300_STATS
			if (accepted && te->s.valid) {
				if (te->iv_count == 0 || interval < te->iv_min) {
					te->iv_min = interval;
				}
				if (te->iv_count == 0 || interval > te->iv_max) {
					te->iv_max = interval;
				}
				te->iv_count++;
			}
#else
			ARG_UNUSED(accepted);
#endif
		}
#ifdef CONFIG_ELCO5300_STATS
		if (te->have_rise) {
			uint64_t tvdh = now - te->rise_cycles;

			te->tvdh_count++;
			te->tvdh_sum += tvdh;
			if (tvdh > te->tvdh_max) {
				te->tvdh_max = tvdh;
			}
		}
		te->falls++;
#endif
		te->fall_cycles = now;
		te->have_fall = true;
		te->fall_seq++;
		give = true;
	} else {
		if (te->have_fall) {
			(void)co5300_est_feed(&te->tvdl, now - te->fall_cycles);
		}
#ifdef CONFIG_ELCO5300_STATS
		if (te->have_fall) {
			uint64_t tvdl = now - te->fall_cycles;

			te->tvdl_count++;
			te->tvdl_sum += tvdl;
			if (tvdl > te->tvdl_max) {
				te->tvdl_max = tvdl;
			}
		}
		te->rises++;
#endif
		te->rise_cycles = now;
		te->have_rise = true;
	}
	k_spin_unlock(&te->lock, key);

	if (give) {
		data->te_seen = true;
		k_sem_give(&data->te_sem);
	}
}

/* The edge rule: blocks until the TE edge that starts a scan, when this write starts a frame.
 * Takes no lock. Runs on the flush thread, the only caller of co5300_write(). */
static void co5300_te_edge_rule(struct co5300_data *data, bool frame_start) {
	if (!data->te_sync || !frame_start || data->blanked) {
		return;
	}

	if (data->te_seen) {
		data->te_seen = false;
		data->te_timeouts_in_row = 0;
	} else if (data->te_timeouts_in_row >= CO5300_TE_MAX_TIMEOUTS) {
		return; /* the line is dead; wait again only after an interrupt was seen */
	}

	uint32_t t0 = k_cycle_get_32();

	k_sem_reset(&data->te_sem);
	int ret = k_sem_take(&data->te_sem, K_MSEC(CO5300_TE_TIMEOUT_MS));

	if (ret != 0) {
		if (++data->te_timeouts_in_row == CO5300_TE_MAX_TIMEOUTS) {
			LOG_WRN("No TE edge for %d frames, frames start without it until one comes",
				CO5300_TE_MAX_TIMEOUTS);
		}
	} else {
		data->te_timeouts_in_row = 0;
	}
#ifdef CONFIG_ELCO5300_STATS
	uint32_t spent = k_cycle_get_32() - t0;
	k_spinlock_key_t key = k_spin_lock(&data->stats_lock);

	data->stats.te_waits++;
	data->stats.te_wait_cycles += spent;
	if (ret != 0) {
		data->stats.te_timeouts++;
	}
	k_spin_unlock(&data->stats_lock, key);
#else
	ARG_UNUSED(t0);
#endif
}
#endif /* CONFIG_ELCO5300_TE_SYNC */

static void co5300_get_capabilities(const struct device *dev, struct display_capabilities *capabilities) {
	const struct co5300_data *data = dev->data;
	const struct co5300_config *config = dev->config;

	memset(capabilities, 0, sizeof(struct display_capabilities));

	/* Exactly what co5300_set_pixel_format() accepts. */
	capabilities->supported_pixel_formats = PIXEL_FORMAT_L_8 | PIXEL_FORMAT_RGB_565X | PIXEL_FORMAT_RGB_888;
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

#ifdef CONFIG_ELCO5300_TE_SYNC
	struct co5300_te *te = &data->te;

	key = k_spin_lock(&te->lock);
	out->te_falls = te->falls;
	out->te_rises = te->rises;
	out->te_coalesced = te->coalesced;
	out->te_s_cycles = te->s.valid ? te->s.cycles : 0;
	out->te_s_valid = te->s.valid;
	out->te_iv_count = te->iv_count;
	out->te_iv_min = te->iv_min;
	out->te_iv_max = te->iv_max;
	out->te_tvdl_count = te->tvdl_count;
	out->te_tvdl_sum = te->tvdl_sum;
	out->te_tvdl_max = te->tvdl_max;
	out->te_tvdh_count = te->tvdh_count;
	out->te_tvdh_sum = te->tvdh_sum;
	out->te_tvdh_max = te->tvdh_max;
	k_spin_unlock(&te->lock, key);
#endif
#ifdef CONFIG_ELCO5300_TE_WINDOW
	const struct co5300_config *config = dev->config;
	uint32_t frame_bytes = (uint32_t)config->width * config->height * data->bytes_per_pixel;

	/* Plain 32-bit reads of what the flush thread updates: good enough for a bench print. */
	co5300_te_wbounds_get(&data->wbounds, frame_bytes, co5300_te_ns_per_byte(data->mspi_dev_config.freq),
		&out->te_win_wmin_us, &out->te_win_wmax_us);
	out->te_win_margin_us = CONFIG_ELCO5300_TE_WINDOW_MARGIN_US;
	out->te_win_on = data->te_window_on;
#endif
}

void co5300_stats_reset(const struct device *dev) {
	struct co5300_data *data = dev->data;
	k_spinlock_key_t key = k_spin_lock(&data->stats_lock);

	memset(&data->stats, 0, sizeof(data->stats));
	data->post_pending = false;
#ifdef CONFIG_ELCO5300_TE_WINDOW
	data->last_full_valid = false;
#endif
	k_spin_unlock(&data->stats_lock, key);

#ifdef CONFIG_ELCO5300_TE_SYNC
	/* The estimate itself keeps running; only what is counted starts again. */
	struct co5300_te *te = &data->te;

	key = k_spin_lock(&te->lock);
	te->falls = 0;
	te->rises = 0;
	te->coalesced = 0;
	te->iv_count = 0;
	te->iv_min = 0;
	te->iv_max = 0;
	te->tvdl_count = 0;
	te->tvdl_sum = 0;
	te->tvdl_max = 0;
	te->tvdh_count = 0;
	te->tvdh_sum = 0;
	te->tvdh_max = 0;
	k_spin_unlock(&te->lock, key);
#endif
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

/* W-only time of one write that succeeded at the first attempt. */
static void co5300_stats_write_w(struct co5300_data *data, bool full, uint32_t cycles) {
	k_spinlock_key_t key = k_spin_lock(&data->stats_lock);
	struct co5300_stats *st = &data->stats;

	if (full) {
		uint32_t us = k_cyc_to_us_floor32(cycles);
		uint32_t bin = us < 22000 ? 0 : us >= 25000 ? CO5300_W_HIST_BINS - 1 : 1 + (us - 22000) / 500;

		if (st->w_full_count == 0 || cycles < st->w_full_min) {
			st->w_full_min = cycles;
		}
		if (cycles > st->w_full_max) {
			st->w_full_max = cycles;
		}
		st->w_full_count++;
		st->w_full_cycles += cycles;
		st->w_full_hist[bin]++;
	} else {
		if (st->w_part_count == 0 || cycles < st->w_part_min) {
			st->w_part_min = cycles;
		}
		if (cycles > st->w_part_max) {
			st->w_part_max = cycles;
		}
		st->w_part_count++;
		st->w_part_cycles += cycles;
	}
	k_spin_unlock(&data->stats_lock, key);
}

static void co5300_stats_count_resent(struct co5300_data *data) {
	k_spinlock_key_t key = k_spin_lock(&data->stats_lock);

	data->stats.w_resent++;
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

/* A re-send of an area after a failed pixel write. */
static void co5300_stats_count_retry(struct co5300_data *data) {
	k_spinlock_key_t key = k_spin_lock(&data->stats_lock);

	data->stats.write_retries++;
	k_spin_unlock(&data->stats_lock, key);
}

static void co5300_stats_count_failure(struct co5300_data *data) {
	k_spinlock_key_t key = k_spin_lock(&data->stats_lock);

	data->stats.write_failures++;
	k_spin_unlock(&data->stats_lock, key);
}

/* One data->lock acquisition by display_write(). */
static void co5300_stats_lock_take(struct co5300_data *data) {
	k_spinlock_key_t key = k_spin_lock(&data->stats_lock);

	data->stats.lock_takes++;
	k_spin_unlock(&data->stats_lock, key);
}
#endif /* CONFIG_ELCO5300_STATS */

#ifdef CONFIG_ELCO5300_TE_WINDOW

#ifdef CONFIG_ELCO5300_STATS
#define CO5300_WIN_STAT_ADD(data, field, n)                                                                        \
	do {                                                                                                           \
		k_spinlock_key_t stat_key_ = k_spin_lock(&(data)->stats_lock);                                             \
		(data)->stats.field += (n);                                                                                \
		k_spin_unlock(&(data)->stats_lock, stat_key_);                                                             \
	} while (0)
#else
#define CO5300_WIN_STAT_ADD(data, field, n) ARG_UNUSED(data)
#endif

/* Rounds of decide-then-wait before the write starts anyway. */
#define CO5300_WIN_MAX_WAITS 4
/* The kernel wakes a k_usleep() on 100 us ticks, up to a tick late: sleep short, spin the rest. */
#define CO5300_WIN_SPIN_US 150U

/* Blanking stops and restarts the scan: the window rule starts from fresh edges and estimates. */
static void co5300_te_restart(struct co5300_data *data) {
	k_spinlock_key_t key = k_spin_lock(&data->te.lock);

	co5300_te_reset(&data->te);
	k_spin_unlock(&data->te.lock, key);
}

/* One reading of the TE state, in microseconds. */
struct co5300_te_snap {
	uint32_t age_us;  /* since the last falling edge */
	uint32_t s_us;
	uint32_t tvdl_us;
	uint32_t seq;     /* falling edges seen */
	bool valid;       /* an edge and both estimates are valid */
};

static uint32_t co5300_us32(uint64_t cycles) {
	uint64_t us = k_cyc_to_us_floor64(cycles);

	return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

static void co5300_te_snapshot(struct co5300_data *data, struct co5300_te_snap *snap) {
	struct co5300_te *te = &data->te;
	k_spinlock_key_t key = k_spin_lock(&te->lock);
	uint64_t now = k_cycle_get_64();
	uint64_t age = now > te->fall_cycles ? now - te->fall_cycles : 0;
	uint64_t s = te->s.cycles;
	uint64_t tvdl = te->tvdl.cycles;

	snap->valid = te->have_fall && te->s.valid && te->tvdl.valid;
	snap->seq = te->fall_seq;
	k_spin_unlock(&te->lock, key);

	snap->age_us = co5300_us32(age);
	snap->s_us = co5300_us32(s);
	snap->tvdl_us = co5300_us32(tvdl);
}

/* The area's rows exist on the panel and the panel scans in the normal direction (T-5). */
static bool co5300_te_geometry_ok(const struct device *dev, const struct display_buffer_descriptor *desc, uint16_t y) {
	const struct co5300_config *config = dev->config;
	const struct co5300_data *data = dev->data;

	return data->orientation == DISPLAY_ORIENTATION_NORMAL && (uint32_t)y + desc->height <= config->height;
}

static uint32_t co5300_area_bytes(const struct co5300_data *data, const struct display_buffer_descriptor *desc) {
	return (uint32_t)desc->height * desc->width * data->bytes_per_pixel;
}

/* Waits us microseconds: sleeps most of it, spins the rest. */
static void co5300_win_wait(uint32_t us) {
	uint64_t deadline = k_cycle_get_64() + k_us_to_cyc_ceil64(us);

	if (us > CO5300_WIN_SPIN_US) {
		k_usleep(us - CO5300_WIN_SPIN_US);
	}
	while (k_cycle_get_64() < deadline) {
	}
}

/* The window rule for one write attempt of the area at row y. Returns true when it placed the start
 * (the caller starts now); false when the edge rule has to decide: no timing, no tear-free start. */
static bool co5300_te_window_gate(const struct device *dev, const struct display_buffer_descriptor *desc, uint16_t y) {
	const struct co5300_config *config = dev->config;
	struct co5300_data *data = dev->data;

	if (!data->te_sync || data->blanked || !data->te_window_on) {
		return false;
	}

	uint32_t bytes = co5300_area_bytes(data, desc);
	uint32_t w_min;
	uint32_t w_max;

	co5300_te_wbounds_get(&data->wbounds, bytes, co5300_te_ns_per_byte(data->mspi_dev_config.freq), &w_min, &w_max);

	bool geometry = co5300_te_geometry_ok(dev, desc, y);
	bool waited = false;
	uint64_t spent = 0;
	bool placed = false;

	for (int round = 0; round <= CO5300_WIN_MAX_WAITS; round++) {
		struct co5300_te_snap snap;

		co5300_te_snapshot(data, &snap);

		struct co5300_te_window_in in = {
			.age_us = snap.age_us,
			.s_us = snap.s_us,
			.tvdl_us = snap.tvdl_us,
			.panel_h = config->height,
			.y0 = y,
			.y1 = (uint16_t)(y + desc->height - 1U),
			.w_min_us = w_min,
			.w_max_us = w_max,
			.margin_us = CONFIG_ELCO5300_TE_WINDOW_MARGIN_US,
			.valid = snap.valid && geometry,
		};
		struct co5300_te_decision dec = co5300_te_window_decide(&in);

		if (dec.action == CO5300_TE_FALLBACK) {
			CO5300_WIN_STAT_ADD(data, te_win_fallbacks, 1U);
			break;
		}
		if (dec.action == CO5300_TE_START_NOW) {
			if (!waited) {
				CO5300_WIN_STAT_ADD(data, te_win_starts_now, 1U);
			}
			placed = true;
			break;
		}
		if (round == CO5300_WIN_MAX_WAITS) {
			CO5300_WIN_STAT_ADD(data, te_win_forced, 1U);
			placed = true;
			break;
		}

		uint64_t t0 = k_cycle_get_64();

		waited = true;
		if (dec.action == CO5300_TE_WAIT) {
			CO5300_WIN_STAT_ADD(data, te_win_waits, 1U);
			co5300_win_wait(dec.wait_us);
		} else {
			struct co5300_te_snap again;

			CO5300_WIN_STAT_ADD(data, te_win_edge_waits, 1U);
			/* An edge between the snapshot and the reset is seen in the sequence number. */
			k_sem_reset(&data->te_sem);
			co5300_te_snapshot(data, &again);
			if (again.seq == snap.seq) {
				int ret = k_sem_take(&data->te_sem, K_USEC(snap.s_us + snap.s_us / 4U));

				if (ret != 0) {
					CO5300_WIN_STAT_ADD(data, te_win_edge_timeouts, 1U);
					if (++data->te_timeouts_in_row == CO5300_TE_MAX_TIMEOUTS) {
						LOG_WRN("No TE edge for %d waits, the edge rule takes over", CO5300_TE_MAX_TIMEOUTS);
					}
				} else {
					data->te_timeouts_in_row = 0;
				}
			}
		}
		spent += k_cycle_get_64() - t0;
	}

#ifdef CONFIG_ELCO5300_STATS
	CO5300_WIN_STAT_ADD(data, te_win_wait_cycles, spent);
#else
	ARG_UNUSED(spent);
#endif
	return placed;
}

#ifdef CONFIG_ELCO5300_STATS
/* What the audit needs from the start of one attempt. */
struct co5300_audit {
	bool valid;
	uint32_t phase_us;
	uint32_t s_us;
	uint32_t t0_us;
	uint32_t t1_us;
	uint32_t w_max_us;  /* the bound the rule used for this start */
	uint32_t margin_us;
};

/* Called right after the attempt's wait (either rule): records the start phase and the rows' times. */
static void co5300_te_audit_begin(const struct device *dev, const struct display_buffer_descriptor *desc, uint16_t y,
				  struct co5300_audit *au) {
	const struct co5300_config *config = dev->config;
	struct co5300_data *data = dev->data;
	struct co5300_te_snap snap;

	au->valid = false;
	if (!data->te_sync || data->blanked || !co5300_te_geometry_ok(dev, desc, y)) {
		return;
	}
	co5300_te_snapshot(data, &snap);
	if (!snap.valid || snap.s_us == 0 || snap.age_us > snap.s_us + snap.s_us / 10U) {
		return;
	}

	au->valid = true;
	au->s_us = snap.s_us;
	au->phase_us = snap.age_us % snap.s_us;
	au->t0_us = co5300_te_row_time(snap.tvdl_us, config->height, y);
	au->t1_us = co5300_te_row_time(snap.tvdl_us, config->height, y + desc->height - 1U);
	au->margin_us = CONFIG_ELCO5300_TE_WINDOW_MARGIN_US;

	uint32_t w_min_unused;

	co5300_te_wbounds_get(&data->wbounds, co5300_area_bytes(data, desc),
			      co5300_te_ns_per_byte(data->mspi_dev_config.freq), &w_min_unused, &au->w_max_us);

	uint32_t bin = (uint32_t)((uint64_t)au->phase_us * CO5300_W_HIST_BINS / au->s_us);

	k_spinlock_key_t key = k_spin_lock(&data->stats_lock);

	data->stats.te_win_phase_hist[bin < CO5300_W_HIST_BINS ? bin : CO5300_W_HIST_BINS - 1U]++;
	k_spin_unlock(&data->stats_lock, key);
}

/* A successful attempt of cycles length: did a scan see it torn? */
static void co5300_te_audit_end(struct co5300_data *data, const struct co5300_audit *au, uint32_t cycles, bool resend) {
	if (!au->valid) {
		return;
	}
	if (co5300_te_audit_late(au->phase_us, k_cyc_to_us_floor32(cycles), au->s_us, au->t0_us, au->t1_us,
				 au->w_max_us, au->margin_us)) {
		if (resend) {
			CO5300_WIN_STAT_ADD(data, te_win_late_resend, 1U);
		} else {
			CO5300_WIN_STAT_ADD(data, te_win_late, 1U);
		}
	}
}

/* The end of a full-frame write: scans since the previous one's end, from the falling-edge count. */
static void co5300_te_frame_end(struct co5300_data *data) {
	uint32_t seq;
	k_spinlock_key_t key = k_spin_lock(&data->te.lock);

	seq = data->te.fall_seq;
	k_spin_unlock(&data->te.lock, key);

	key = k_spin_lock(&data->stats_lock);
	if (data->last_full_valid) {
		uint32_t n = seq - data->last_full_seq;

		/* No falling edge since the last frame (TE off, a capture, a lost edge): no scan count. */
		if (n >= 1U) {
			data->stats.te_win_scans_hist[n >= 5U ? 4U : n - 1U]++;
		}
	}
	data->last_full_valid = true;
	data->last_full_seq = seq;
	k_spin_unlock(&data->stats_lock, key);
}
#endif /* CONFIG_ELCO5300_STATS */

#endif /* CONFIG_ELCO5300_TE_WINDOW */

#ifdef CONFIG_ELCO5300_TE_SYNC
/* One write attempt's wait: the window rule, and the edge rule where it hands over. */
static void co5300_te_gate(const struct device *dev, const struct display_buffer_descriptor *desc, uint16_t y,
			   bool frame_start) {
#ifdef CONFIG_ELCO5300_TE_WINDOW
	if (co5300_te_window_gate(dev, desc, y)) {
		return;
	}
#else
	ARG_UNUSED(desc);
	ARG_UNUSED(y);
#endif
	co5300_te_edge_rule(dev->data, frame_start);
}
#endif

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
	/* A pixel failure is logged once by co5300_write() (it retries); each line is printed
	 * synchronously on the flush thread's small stack. */
	if (ret < 0 && kind != CO5300_STAT_PIXEL) {
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
#ifdef CONFIG_ELCO5300_TE_SYNC
	data->blanked = false;
	data->frame_start = true;
#ifdef CONFIG_ELCO5300_TE_WINDOW
	co5300_te_restart(data);
#endif
#endif

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
#ifdef CONFIG_ELCO5300_TE_SYNC
	data->blanked = true;
	data->frame_start = true;
#ifdef CONFIG_ELCO5300_TE_WINDOW
	co5300_te_restart(data);
#endif
#endif

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
	} else if (pixel_format == PIXEL_FORMAT_RGB_565X) {
		/* The panel reads big-endian RGB565; the little-endian PIXEL_FORMAT_RGB_565 would show swapped colours. */
		bytes_per_pixel = 2U;
		tx_data = CO5300_PIXFMT_16BIT_RGB565;
	} else if (pixel_format == PIXEL_FORMAT_RGB_888) {
		bytes_per_pixel = 3U;
		tx_data = CO5300_PIXFMT_24BIT_RGB888;
	} else {
		LOG_ERR("Unsupported pixel format 0x%x (supported: L_8, RGB_565X, RGB_888)", (unsigned int)pixel_format);
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
		/* co5300_set_window() adds the same offsets as at normal. A panel mirrored over the
		 * controller's 480 columns but 466 wide may need another x-offset here (unverified). */
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
	return ret;
}

/* Extra attempts after a pixel write fails with -EIO (PSRAM underrun) or -ETIMEDOUT. The MSPI
 * driver stops the packet there and LVGL never redraws the area (partial mode), so the panel
 * would keep a truncated piece. */
#define CO5300_WRITE_RETRIES 2

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

#ifdef CONFIG_ELCO5300_TE_SYNC
	bool frame_start = data->frame_start;

	data->frame_start = !desc->frame_incomplete;
	co5300_te_gate(dev, desc, y, frame_start);
#endif

#if defined(CONFIG_ELCO5300_TE_WINDOW) && defined(CONFIG_ELCO5300_STATS)
	struct co5300_audit audit;
#endif
#if defined(CONFIG_ELCO5300_STATS) || defined(CONFIG_ELCO5300_TE_WINDOW)
	uint32_t w_cycles = 0;
#endif

	ret = co5300_lock(data);
	if (ret < 0) {
		return ret;
	}
	/* After the lock: the wait for the mutex (blanking holds it for 130 or 240 ms) is not write time,
	 * and the audit's start is the first pixel's, not the request's. */
#if defined(CONFIG_ELCO5300_TE_WINDOW) && defined(CONFIG_ELCO5300_STATS)
	co5300_te_audit_begin(dev, desc, y, &audit);
#endif
#if defined(CONFIG_ELCO5300_STATS) || defined(CONFIG_ELCO5300_TE_WINDOW)
	uint32_t w_start = k_cycle_get_32();
#endif
#ifdef CONFIG_ELCO5300_STATS
	co5300_stats_lock_take(data);
	bool resent = false;
#endif

	/* The buffer stays valid until we return, so the whole area (window and packet) can be re-sent. */
	ret = co5300_write_locked(dev, x, y, desc, buf);
#if defined(CONFIG_ELCO5300_STATS) || defined(CONFIG_ELCO5300_TE_WINDOW)
	w_cycles = k_cycle_get_32() - w_start;
#endif
#ifdef CONFIG_ELCO5300_TE_WINDOW
	if (ret == 0) {
		/* Only a first attempt that succeeded teaches the bounds. */
		co5300_te_wbounds_update(&data->wbounds, co5300_area_bytes(data, desc),
			co5300_te_ns_per_byte(data->mspi_dev_config.freq), k_cyc_to_us_floor32(w_cycles));
#ifdef CONFIG_ELCO5300_STATS
		co5300_te_audit_end(data, &audit, w_cycles, false);
#endif
	}
#endif
	for (int attempt = 0; (ret == -EIO || ret == -ETIMEDOUT) && attempt < CO5300_WRITE_RETRIES; attempt++) {
		LOG_WRN("Pixel write failed (%d), re-sending the area (%d/%d)", ret, attempt + 1,
			CO5300_WRITE_RETRIES);
		co5300_forget_window(data);
#ifdef CONFIG_ELCO5300_STATS
		co5300_stats_count_retry(data);
		resent = true;
#endif
#ifdef CONFIG_ELCO5300_TE_WINDOW
		/* A re-send starts at whatever phase the failure left: the rule places it too. */
		co5300_te_gate(dev, desc, y, frame_start);
#ifdef CONFIG_ELCO5300_STATS
		co5300_te_audit_begin(dev, desc, y, &audit);
#endif
		uint32_t r_start = k_cycle_get_32();
#endif
		ret = co5300_write_locked(dev, x, y, desc, buf);
#if defined(CONFIG_ELCO5300_TE_WINDOW) && defined(CONFIG_ELCO5300_STATS)
		if (ret == 0) {
			co5300_te_audit_end(data, &audit, k_cycle_get_32() - r_start, true);
		}
#elif defined(CONFIG_ELCO5300_TE_WINDOW)
		ARG_UNUSED(r_start);
#endif
	}
	if (ret < 0) {
		co5300_forget_window(data);
		if (ret == -EIO || ret == -ETIMEDOUT) {
			LOG_ERR("Pixel write failed on every attempt (%d): the area is left incomplete", ret);
#ifdef CONFIG_ELCO5300_STATS
			co5300_stats_count_failure(data);
#endif
		}
	}

	co5300_unlock(data);

#ifdef CONFIG_ELCO5300_STATS
	if (ret == 0) {
		const struct co5300_config *config = dev->config;
		bool full = (uint32_t)desc->width * desc->height == (uint32_t)config->width * config->height;

		if (resent) {
			co5300_stats_count_resent(data);
		} else {
			co5300_stats_write_w(data, full, w_cycles);
		}
#ifdef CONFIG_ELCO5300_TE_WINDOW
		if (full) {
			co5300_te_frame_end(data);
		}
#endif
	}
	co5300_stats_write_exit(data, start);
#endif

	return ret;
}

#if defined(CONFIG_ELCO5300_STATS) && defined(CONFIG_ELCO5300_TE_SYNC)

int co5300_te_irq_enable(const struct device *dev, bool enable) {
	const struct co5300_config *config = dev->config;
	struct co5300_data *data = dev->data;

	if (!data->te_sync) {
		return -ENOTSUP;
	}

	int ret = gpio_pin_interrupt_configure_dt(&config->te_gpio, enable ? GPIO_INT_EDGE_BOTH : GPIO_INT_DISABLE);

	if (ret == 0) {
		/* Edges missed meanwhile: nothing to compare the next one with. */
		k_spinlock_key_t key = k_spin_lock(&data->te.lock);

		co5300_te_forget_edges(&data->te);
		k_spin_unlock(&data->te.lock, key);
	}
	return ret;
}

#ifdef CONFIG_ELCO5300_TE_WINDOW
int co5300_te_window_enable(const struct device *dev, bool enable) {
	struct co5300_data *data = dev->data;

	if (!data->te_sync) {
		return -ENOTSUP;
	}
	data->te_window_on = enable;
	return 0;
}
#endif

int co5300_te_set_scanline(const struct device *dev, uint16_t line) {
	struct co5300_data *data = dev->data;
	uint8_t cmd;
	uint8_t params[2];
	size_t len;

	if (!data->te_sync) {
		return -ENOTSUP;
	}

	/* Line 0 is sent as STESL too: TEON cannot leave the scanline mode (it has no effect with TE
	 * already on), while STESL with N = 0 is the documented way back to mode 1. */
	cmd = CO5300_W_SETTSL;
	params[0] = line >> 8;
	params[1] = line & 0xFF;
	len = 2;

	int ret = co5300_lock(data);

	if (ret < 0) {
		return ret;
	}
	ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, params, len, false);
	co5300_unlock(data);
	return ret;
}

int co5300_te_capture_start(const struct device *dev, struct co5300_te_sample *buf, size_t count) {
	struct co5300_data *data = dev->data;
	struct co5300_te *te = &data->te;

	if (!data->te_sync) {
		return -ENOTSUP;
	}
	if (buf == NULL || count == 0) {
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&te->lock);

	if (te->cap_buf != NULL) {
		k_spin_unlock(&te->lock, key);
		return -EBUSY;
	}
	te->cap_count = count;
	te->cap_n = 0;
	k_sem_reset(&te->cap_sem);
	te->cap_buf = buf; /* arms it: the ISR sees this under the lock */
	k_spin_unlock(&te->lock, key);
	return 0;
}

int co5300_te_capture_wait(const struct device *dev, k_timeout_t timeout, size_t *got) {
	struct co5300_data *data = dev->data;
	struct co5300_te *te = &data->te;

	if (!data->te_sync) {
		return -ENOTSUP;
	}

	int ret = k_sem_take(&te->cap_sem, timeout);
	k_spinlock_key_t key = k_spin_lock(&te->lock);

	/* The ISR may have filled the buffer between the timeout and here. */
	if (te->cap_buf != NULL) {
		te->cap_buf = NULL;
		ret = -ETIMEDOUT;
	} else if (ret != 0) {
		ret = 0;
	}
	if (got != NULL) {
		*got = te->cap_n;
	}
	/* The edges around the capture were not seen by the estimator. */
	co5300_te_forget_edges(te);
	k_spin_unlock(&te->lock, key);
	return ret;
}
#elif defined(CONFIG_ELCO5300_STATS)
int co5300_te_irq_enable(const struct device *dev, bool enable) {
	ARG_UNUSED(dev);
	ARG_UNUSED(enable);
	return -ENOTSUP;
}

int co5300_te_set_scanline(const struct device *dev, uint16_t line) {
	ARG_UNUSED(dev);
	ARG_UNUSED(line);
	return -ENOTSUP;
}

int co5300_te_capture_start(const struct device *dev, struct co5300_te_sample *buf, size_t count) {
	ARG_UNUSED(dev);
	ARG_UNUSED(buf);
	ARG_UNUSED(count);
	return -ENOTSUP;
}

int co5300_te_capture_wait(const struct device *dev, k_timeout_t timeout, size_t *got) {
	ARG_UNUSED(dev);
	ARG_UNUSED(timeout);
	ARG_UNUSED(got);
	return -ENOTSUP;
}
#endif /* CONFIG_ELCO5300_STATS */

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
	uint8_t tear_mode = 0x00; // M = 0: V-blanking information only, high in the porch, low while scanning
	ret = co5300_transmit(dev, MSPI_IO_MODE_SINGLE, &cmd, &tear_mode, 1, false);
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
#ifdef CONFIG_ELCO5300_TE_SYNC
		/* TE mode 1 (M = 0): high in the V-porch, low while the panel scans; the scan starts on the
		 * falling edge. Both edges interrupt: the ISR tells them apart by the pin level. */
		k_sem_init(&data->te_sem, 0, 1);
#ifdef CONFIG_ELCO5300_STATS
		k_sem_init(&data->te.cap_sem, 0, 1);
#endif
		data->dev = dev;
		data->frame_start = true;
#ifdef CONFIG_ELCO5300_TE_WINDOW
		data->te_window_on = true;
		co5300_te_wbounds_init(&data->wbounds);
#endif
		gpio_init_callback(&data->te_sync_cb_data, co5300_te_isr, BIT(config->te_gpio.pin));
		ret = gpio_add_callback(config->te_gpio.port, &data->te_sync_cb_data);
		if (ret == 0) {
			ret = gpio_pin_interrupt_configure_dt(&config->te_gpio, GPIO_INT_EDGE_BOTH);
		}
		if (ret < 0) {
			LOG_ERR("Failed to set up TE sync (%d)", ret);
			return ret;
		}
		data->te_sync = true;
#endif
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
