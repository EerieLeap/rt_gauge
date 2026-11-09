/*
 * Copyright (c) 2025 Pavel Maloletkov.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The CO5300 window rule: when may a write of rows y0-y1 start, relative to the panel's scan, so that
 * no scan sees part of the area old and part new (no tear)?
 *
 * Pure functions on plain integers, in microseconds; no Zephyr includes, so the unit test builds this
 * header on its own (tests/unit/te_window). Intermediate products use 64 bits, so the 32-bit inputs
 * may be several seconds without overflow.
 *
 * Model (plan DISPLAY_PERFORMANCE_NEXT_PLAN.md, T-2). A scan starts at a falling edge of TE and reads
 * row y at t(y) = y * tvdl / panel_h after it. A write of an area takes W, writing its rows linearly.
 * A start phase (time since the last falling edge, modulo the scan period S) is forbidden when a scan
 * would overtake the writer, or the writer the scan, inside the area:
 *   writer slower than the scan: (t(y1) - W, t(y0))
 *   writer faster than the scan: (t(y0), t(y1) - W)
 * With W only known within [w_min, w_max], the forbidden interval is (a, b):
 *   a = min(t(y1) - w_max, t(y0)) - margin
 *   b = max(t(y0), t(y1) - w_min) + margin
 * periodic in S. Starting exactly at b is allowed.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum co5300_te_action {
	CO5300_TE_START_NOW,  /* start the write now */
	CO5300_TE_WAIT,       /* wait wait_us, then decide again */
	CO5300_TE_WAIT_EDGE,  /* the wait crosses the next falling edge: wait for it, then decide again */
	CO5300_TE_FALLBACK,   /* no timing, or no tear-free start exists: use the plain edge rule */
};

struct co5300_te_window_in {
	uint32_t age_us;      /* time since the last falling edge */
	uint32_t s_us;        /* scan period */
	uint32_t tvdl_us;     /* TE low time, which holds the panel's rows */
	uint16_t panel_h;     /* rows of the panel */
	uint16_t y0;          /* first row of the area */
	uint16_t y1;          /* last row of the area, y0 <= y1 < panel_h */
	uint32_t w_min_us;    /* bounds of this area's write time, w_min <= w_max */
	uint32_t w_max_us;
	uint32_t margin_us;   /* widens the forbidden interval on both sides */
	bool valid;           /* S and tvdl are validated, the edge is fresh, normal orientation */
};

struct co5300_te_decision {
	enum co5300_te_action action;
	uint32_t wait_us;     /* WAIT: the wait; WAIT_EDGE: the time to the next edge */
};

/* Row y of the panel is read this long after the falling edge. */
static inline uint32_t co5300_te_row_time(uint32_t tvdl_us, uint16_t panel_h, uint16_t y) {
	return panel_h == 0 ? 0 : (uint32_t)(((uint64_t)y * tvdl_us) / panel_h);
}

/* x modulo m for any x, in [0, m). */
static inline int64_t co5300_te_mod(int64_t x, int64_t m) {
	int64_t r = x % m;

	return r < 0 ? r + m : r;
}

static inline struct co5300_te_decision co5300_te_window_decide(const struct co5300_te_window_in *in) {
	struct co5300_te_decision out = {CO5300_TE_FALLBACK, 0};

	if (!in->valid || in->s_us == 0 || in->tvdl_us == 0 || in->tvdl_us > in->s_us || in->panel_h == 0 ||
	    in->y0 > in->y1 || in->y1 >= in->panel_h || in->w_min_us > in->w_max_us) {
		return out;
	}
	/* A stale edge (flash operations, a stopped scan): the age says nothing about the phase. */
	if ((uint64_t)in->age_us > (uint64_t)in->s_us + in->s_us / 10) {
		return out;
	}

	int64_t s = in->s_us;
	int64_t t0 = co5300_te_row_time(in->tvdl_us, in->panel_h, in->y0);
	int64_t t1 = co5300_te_row_time(in->tvdl_us, in->panel_h, in->y1);
	int64_t lo = t1 - (int64_t)in->w_max_us;
	int64_t hi = t1 - (int64_t)in->w_min_us;
	int64_t a = (lo < t0 ? lo : t0) - (int64_t)in->margin_us;
	int64_t b = (hi > t0 ? hi : t0) + (int64_t)in->margin_us;
	int64_t width = b - a;

	if (width >= s) {
		return out;
	}

	int64_t phase = (int64_t)in->age_us % s;
	int64_t d = co5300_te_mod(phase - a, s);

	if (d >= width) {
		out.action = CO5300_TE_START_NOW;
		return out;
	}

	int64_t wait = width - d;

	if (phase + wait >= s) {
		out.action = CO5300_TE_WAIT_EDGE;
		out.wait_us = (uint32_t)(s - phase);
	} else {
		out.action = CO5300_TE_WAIT;
		out.wait_us = (uint32_t)wait;
	}
	return out;
}

/*
 * The self-audit: did a write that began at start_phase_us after a falling edge and took w_us tear?
 * The writer writes the area's rows linearly from the start to the end; the scanner reads row y at
 * k * S + t(y). A scan tears the area when it sees some rows old and some new; both are linear in the
 * row, so comparing the first and the last row finds any crossing of a writer that is a straight line
 * from start_phase_us (the first pixel) to the end. Meeting exactly is no tear.
 * t0_us and t1_us are t(y0) and t(y1). A writer that stalled (a gap between pieces) is no straight
 * line: use co5300_te_audit_late(), which also counts a write longer than expected as late.
 */
static inline bool co5300_te_audit_torn(uint32_t start_phase_us, uint32_t w_us, uint32_t s_us, uint32_t t0_us,
					uint32_t t1_us) {
	if (s_us == 0) {
		return false;
	}

	int64_t start = start_phase_us;
	int64_t end = start + (int64_t)w_us;
	int64_t s = s_us;

	for (int64_t k = 0; k * s <= end + s; k++) {
		/* Positive: the writer reaches the row after the scan read it, the scan shows it old. */
		int64_t d0 = start - (k * s + (int64_t)t0_us);
		int64_t d1 = end - (k * s + (int64_t)t1_us);

		if ((d0 < 0 && d1 > 0) || (d0 > 0 && d1 < 0)) {
			return true;
		}
	}
	return false;
}

/*
 * The audit's verdict: torn as a straight line, or too long to be proven tear-free from two rows.
 * w_max_us is the upper bound the rule used for this start (read before the write taught the bounds);
 * a write longer than w_max_us + margin_us had a stall the rule did not allow for.
 */
static inline bool co5300_te_audit_late(uint32_t start_phase_us, uint32_t w_us, uint32_t s_us, uint32_t t0_us,
					uint32_t t1_us, uint32_t w_max_us, uint32_t margin_us) {
	if (s_us == 0) {
		return false;
	}
	if ((uint64_t)w_us > (uint64_t)w_max_us + margin_us) {
		return true;
	}
	return co5300_te_audit_torn(start_phase_us, w_us, s_us, t0_us, t1_us);
}

/*
 * Write-time bounds per area size class.
 *
 * W_model(bytes) = 300 us + bytes * ns_per_byte / 1000 + floor((bytes - 1) / 32768) * 115 us
 * The measured W minus the model is tracked as two envelopes per class: the maximum follows up at
 * once and decays by 1/128 of the distance per sample (at least 1 us), the minimum mirrored.
 * Thread context, no locking: one writer.
 */
#define CO5300_TE_W_CLASSES 3
#define CO5300_TE_W_SMALL_BYTES 16384U
#define CO5300_TE_W_LARGE_BYTES 131072U
#define CO5300_TE_W_UNSEEN_MAX_US 600
#define CO5300_TE_W_UNSEEN_MIN_US (-150)

struct co5300_te_wclass {
	bool have;
	int32_t env_min_us;
	int32_t env_max_us;
};

struct co5300_te_wbounds {
	struct co5300_te_wclass cls[CO5300_TE_W_CLASSES];
};

static inline unsigned co5300_te_w_class(uint32_t bytes) {
	return bytes < CO5300_TE_W_SMALL_BYTES ? 0U : bytes < CO5300_TE_W_LARGE_BYTES ? 1U : 2U;
}

/* Nanoseconds per byte on a quad bus (8 bits over 4 lines = 2 clocks) at freq_hz. */
static inline uint32_t co5300_te_ns_per_byte(uint32_t freq_hz) {
	return freq_hz == 0 ? 50U : (uint32_t)((2000000000ULL + freq_hz / 2) / freq_hz);
}

static inline uint32_t co5300_te_w_model_us(uint32_t bytes, uint32_t ns_per_byte) {
	uint64_t us = 300ULL + (uint64_t)bytes * ns_per_byte / 1000ULL;

	if (bytes > 0) {
		us += (uint64_t)((bytes - 1U) / 32768U) * 115ULL;
	}
	return us > UINT32_MAX ? UINT32_MAX : (uint32_t)us;
}

static inline void co5300_te_wbounds_init(struct co5300_te_wbounds *wb) {
	for (unsigned i = 0; i < CO5300_TE_W_CLASSES; i++) {
		wb->cls[i].have = false;
		wb->cls[i].env_min_us = 0;
		wb->cls[i].env_max_us = 0;
	}
}

/* Feeds one successful write: w_us measured, for an area of that many bytes. */
static inline void co5300_te_wbounds_update(struct co5300_te_wbounds *wb, uint32_t bytes, uint32_t ns_per_byte,
					    uint32_t w_us) {
	struct co5300_te_wclass *c = &wb->cls[co5300_te_w_class(bytes)];
	int64_t diff = (int64_t)w_us - (int64_t)co5300_te_w_model_us(bytes, ns_per_byte);
	int32_t sample = diff > INT32_MAX ? INT32_MAX : diff < INT32_MIN ? INT32_MIN : (int32_t)diff;

	if (!c->have) {
		c->have = true;
		c->env_min_us = sample;
		c->env_max_us = sample;
		return;
	}
	if (sample >= c->env_max_us) {
		c->env_max_us = sample;
	} else {
		int64_t dist = (int64_t)c->env_max_us - sample;
		int64_t step = dist / 128 > 1 ? dist / 128 : 1;

		c->env_max_us = (int32_t)((int64_t)c->env_max_us - step);
	}
	if (sample <= c->env_min_us) {
		c->env_min_us = sample;
	} else {
		int64_t dist = (int64_t)sample - c->env_min_us;
		int64_t step = dist / 128 > 1 ? dist / 128 : 1;

		c->env_min_us = (int32_t)((int64_t)c->env_min_us + step);
	}
}

static inline void co5300_te_wbounds_get(const struct co5300_te_wbounds *wb, uint32_t bytes, uint32_t ns_per_byte,
					 uint32_t *w_min_us, uint32_t *w_max_us) {
	const struct co5300_te_wclass *c = &wb->cls[co5300_te_w_class(bytes)];
	int64_t model = co5300_te_w_model_us(bytes, ns_per_byte);
	int64_t lo = model + (c->have ? c->env_min_us : CO5300_TE_W_UNSEEN_MIN_US);
	int64_t hi = model + (c->have ? c->env_max_us : CO5300_TE_W_UNSEEN_MAX_US);

	if (lo < 1) {
		lo = 1;
	}
	if (hi < lo) {
		hi = lo;
	}
	*w_min_us = lo > UINT32_MAX ? UINT32_MAX : (uint32_t)lo;
	*w_max_us = hi > UINT32_MAX ? UINT32_MAX : (uint32_t)hi;
}

#ifdef __cplusplus
}
#endif
