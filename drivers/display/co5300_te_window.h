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
#include <stddef.h>
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

/*
 * The forbidden start interval (a, b) of rows y0-y1 for a write of w_min..w_max, relative to the falling
 * edge and periodic in S: a = min(t(y1) - w_max, t(y0)) - margin, b = max(t(y0), t(y1) - w_min) + margin.
 * The one rule behind the decide below and every window of a frame plan.
 */
static inline void co5300_te_window_bounds(uint32_t tvdl_us, uint16_t panel_h, uint16_t y0, uint16_t y1,
					   uint32_t w_min_us, uint32_t w_max_us, uint32_t margin_us, int64_t *a_us,
					   int64_t *b_us) {
	int64_t t0 = co5300_te_row_time(tvdl_us, panel_h, y0);
	int64_t t1 = co5300_te_row_time(tvdl_us, panel_h, y1);
	int64_t lo = t1 - (int64_t)w_max_us;
	int64_t hi = t1 - (int64_t)w_min_us;

	*a_us = (lo < t0 ? lo : t0) - (int64_t)margin_us;
	*b_us = (hi > t0 ? hi : t0) + (int64_t)margin_us;
}

static inline struct co5300_te_decision co5300_te_window_decide(const struct co5300_te_window_in *in) {
	struct co5300_te_decision out = {CO5300_TE_FALLBACK, 0};

	if (!in->valid || in->s_us == 0 || in->tvdl_us == 0 || in->tvdl_us > in->s_us || in->panel_h == 0 ||
	    in->y0 > in->y1 || in->y1 >= in->panel_h || in->w_min_us > in->w_max_us) {
		return out;
	}
	/* A stale edge (a stopped scan, interrupts held off for long): the age says nothing about the phase. */
	if ((uint64_t)in->age_us > (uint64_t)in->s_us + in->s_us / 10) {
		return out;
	}

	int64_t s = in->s_us;
	int64_t a;
	int64_t b;

	co5300_te_window_bounds(in->tvdl_us, in->panel_h, in->y0, in->y1, in->w_min_us, in->w_max_us, in->margin_us,
				&a, &b);

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


/*
 * ---- Frames: several rects for one target scan (plan FRAME_CONSISTENT_WRITES_PLAN.md, sections 5 and 6.3) ----
 *
 * Times are relative to the falling edge E_0 of a TE snapshot: scan n starts at n * S. Rect i with the
 * forbidden interval (a_i, b_i) is first shown by scan N, untorn, exactly when its write starts in
 *   L_i(N) = (N - 1) * S + b_i  <=  s_i  <  U_i(N) = N * S + a_i.
 * The planner picks the order and N; the driver then asks co5300_te_frame_step() before every rect.
 * No recursion and no allocation: the order search keeps its state in the caller's scratch.
 */

#define CO5300_TE_FRAME_GAP_US       50U  /* guessed: rect end to next start inside a batch */
#define CO5300_TE_FRAME_SEARCH_MAX   6    /* rects up to which the order is searched */
#define CO5300_TE_FRAME_SEARCH_NODES 2000 /* search nodes per frame, over every target scan tried */
#define CO5300_TE_FRAME_MAX_REPLANS  2    /* then the rest of the frame goes as without timing */
#define CO5300_TE_FRAME_MAX_WAITS    4    /* wait rounds per rect; running out is a miss */

/* x / m rounded down, for any x and m > 0. */
static inline int64_t co5300_te_fdiv(int64_t x, int64_t m) {
	int64_t q = x / m;

	return (x % m < 0) ? q - 1 : q;
}

struct co5300_te_frame_in {
	uint32_t age_us;          /* since the snapshot's falling edge E_0 (scan 0 started there) */
	uint32_t s_us;
	uint32_t tvdl_us;
	uint16_t panel_h;
	uint32_t margin_us;
	uint32_t gap_us;
	int32_t min_scan;         /* N_min from the bound, relative to E_0; 0 for none */
	bool valid;               /* as co5300_te_window_in.valid */
	uint8_t n;
	const uint16_t *y0;       /* n entries each */
	const uint16_t *y1;
	const uint32_t *w_min_us; /* frozen at plan time */
	const uint32_t *w_max_us;
};

struct co5300_te_frame_scratch {  /* caller-owned; the driver points it into co5300_frame_work */
	int64_t *a_us;                 /* n entries each */
	int64_t *b_us;
	uint8_t *best;
	uint8_t *stack;                /* CO5300_TE_FRAME_SEARCH_MAX entries each */
	int64_t *stack_t_us;
};

enum co5300_te_frame_result {
	CO5300_TE_FRAME_OK,        /* order[] and *scan are set */
	CO5300_TE_FRAME_NO_WINDOW, /* no order and scan hold every rect, from any phase: write it split */
	CO5300_TE_FRAME_FALLBACK,  /* no timing or bad input: the edge rule */
};

/* The start window of a rect with bounds (a, b) for target scan n: [lo, hi), relative to the edge. */
static inline void co5300_te_frame_window(int64_t a_us, int64_t b_us, uint32_t s_us, int32_t n, int64_t *lo_us,
					  int64_t *hi_us) {
	*lo_us = ((int64_t)n - 1) * (int64_t)s_us + b_us;
	*hi_us = (int64_t)n * (int64_t)s_us + a_us;
}

/* The default order: by b, then y0, then index. */
static inline void co5300_te_frame_sorted(const struct co5300_te_frame_in *in,
					  const struct co5300_te_frame_scratch *sc, uint8_t *order) {
	for (uint8_t i = 0; i < in->n; i++) {
		uint8_t j = i;

		while (j > 0) {
			uint8_t p = order[j - 1];
			bool before = sc->b_us[i] < sc->b_us[p] ||
				      (sc->b_us[i] == sc->b_us[p] && in->y0[i] < in->y0[p]);

			if (!before) {
				break;
			}
			order[j] = p;
			j--;
		}
		order[j] = i;
	}
}

/* Section 5.3: the earliest starts of order[] for target N. Feasible when every start is before its U. */
static inline bool co5300_te_frame_follow(const struct co5300_te_frame_in *in,
					  const struct co5300_te_frame_scratch *sc, const uint8_t *order, int32_t scan,
					  int64_t *end_us) {
	int64_t t = in->age_us;
	int64_t end = t;

	for (uint8_t k = 0; k < in->n; k++) {
		uint8_t i = order[k];
		int64_t lo;
		int64_t hi;

		co5300_te_frame_window(sc->a_us[i], sc->b_us[i], in->s_us, scan, &lo, &hi);

		int64_t start = t > lo ? t : lo;

		if (start >= hi) {
			return false;
		}
		end = start + (int64_t)in->w_max_us[i];
		t = end + (int64_t)in->gap_us;
	}
	*end_us = end;
	return true;
}

/*
 * Depth first over the orders of n <= SEARCH_MAX rects for target N, on an explicit stack. A prefix is
 * dropped as soon as a start misses its U or cannot end before the best order so far. Keeps the order
 * with the earliest end, ties to the lower index. Every rect tried counts one node of the frame's budget.
 * The best order is written to out[].
 */
static inline bool co5300_te_frame_search(const struct co5300_te_frame_in *in, struct co5300_te_frame_scratch *sc,
					  int32_t scan, uint8_t *out, uint32_t *nodes) {
	const uint8_t n = in->n;
	uint32_t used = 0;
	int level = 0;
	bool found = false;
	int64_t best_end = 0;

	sc->stack[0] = 0;
	while (level >= 0) {
		if (sc->stack[level] >= n) {
			level--;
			if (level >= 0) {
				used &= ~(1U << (sc->stack[level] - 1U));
			}
			continue;
		}

		uint8_t c = sc->stack[level]++;

		if ((used & (1U << c)) != 0U) {
			continue;
		}
		if (*nodes >= CO5300_TE_FRAME_SEARCH_NODES) {
			break;
		}
		(*nodes)++;

		int64_t t = level == 0 ? (int64_t)in->age_us : sc->stack_t_us[level - 1];
		int64_t lo;
		int64_t hi;

		co5300_te_frame_window(sc->a_us[c], sc->b_us[c], in->s_us, scan, &lo, &hi);

		int64_t start = t > lo ? t : lo;

		if (start >= hi) {
			continue;
		}

		int64_t end = start + (int64_t)in->w_max_us[c];

		if (found && end >= best_end) {
			continue;
		}
		if (level + 1 == (int)n) {
			found = true;
			best_end = end;
			for (int k = 0; k < level; k++) {
				out[k] = (uint8_t)(sc->stack[k] - 1U);
			}
			out[level] = c;
			continue;
		}
		sc->stack_t_us[level] = end + (int64_t)in->gap_us;
		used |= 1U << c;
		level++;
		sc->stack[level] = 0;
	}
	return found;
}

/* One target scan: the default order, then the search if that fails. out[] holds the order on success. */
static inline bool co5300_te_frame_try_scan(const struct co5300_te_frame_in *in,
					    struct co5300_te_frame_scratch *sc, int32_t scan, uint8_t *out,
					    uint32_t *nodes) {
	int64_t end;

	co5300_te_frame_sorted(in, sc, out);
	if (co5300_te_frame_follow(in, sc, out, scan, &end)) {
		return true;
	}
	if (in->n < 2 || in->n > CO5300_TE_FRAME_SEARCH_MAX) {
		return false;
	}
	return co5300_te_frame_search(in, sc, scan, out, nodes);
}

/*
 * Sections 5.4-5.5. N_last, the first scan whose every window opens after the age, is checked first: from
 * there on the schedules differ only by a shift of S, so a failure means no phase has a window
 * (NO_WINDOW). Then N_min .. N_last - 1 in increasing order; the first feasible one wins, else N_last.
 * *nodes returns the search nodes spent, never more than SEARCH_NODES for the whole frame.
 */
static inline enum co5300_te_frame_result co5300_te_frame_plan(const struct co5300_te_frame_in *in,
							       struct co5300_te_frame_scratch *scratch,
							       uint8_t *order, int32_t *scan, uint32_t *nodes) {
	*nodes = 0;
	if (!in->valid || in->n == 0 || in->s_us == 0 || in->tvdl_us == 0 || in->tvdl_us > in->s_us ||
	    in->panel_h == 0) {
		return CO5300_TE_FRAME_FALLBACK;
	}
	/* A stale edge: the age says nothing about the phase (as the decide). */
	if ((uint64_t)in->age_us > (uint64_t)in->s_us + in->s_us / 10) {
		return CO5300_TE_FRAME_FALLBACK;
	}

	const int64_t s = in->s_us;
	bool no_window = false;
	int64_t min_b = 0;

	for (uint8_t i = 0; i < in->n; i++) {
		if (in->y0[i] > in->y1[i] || in->y1[i] >= in->panel_h || in->w_min_us[i] > in->w_max_us[i]) {
			return CO5300_TE_FRAME_FALLBACK;
		}
		co5300_te_window_bounds(in->tvdl_us, in->panel_h, in->y0[i], in->y1[i], in->w_min_us[i],
					in->w_max_us[i], in->margin_us, &scratch->a_us[i], &scratch->b_us[i]);
		if (scratch->b_us[i] - scratch->a_us[i] >= s) {
			no_window = true;
		}
		if (i == 0 || scratch->b_us[i] < min_b) {
			min_b = scratch->b_us[i];
		}
	}
	if (no_window) {
		return CO5300_TE_FRAME_NO_WINDOW;
	}

	int64_t n_min = in->min_scan > 0 ? in->min_scan : 0;
	/* The first N with (N - 1) * S + min_b >= age */
	int64_t n_last = -co5300_te_fdiv(-((int64_t)in->age_us - min_b), s) + 1;

	if (n_last < n_min) {
		n_last = n_min;
	}
	if (!co5300_te_frame_try_scan(in, scratch, (int32_t)n_last, scratch->best, nodes)) {
		return CO5300_TE_FRAME_NO_WINDOW;
	}
	for (int64_t n = n_min; n < n_last; n++) {
		if (co5300_te_frame_try_scan(in, scratch, (int32_t)n, order, nodes)) {
			*scan = (int32_t)n;
			return CO5300_TE_FRAME_OK;
		}
	}
	for (uint8_t k = 0; k < in->n; k++) {
		order[k] = scratch->best[k];
	}
	*scan = (int32_t)n_last;
	return CO5300_TE_FRAME_OK;
}

/* What to do next for order[k] (section 5.9). */
enum co5300_te_frame_action {
	CO5300_TE_FRAME_START,     /* start order[k] now */
	CO5300_TE_FRAME_WAIT,      /* sleep *wait_us, then step again */
	CO5300_TE_FRAME_WAIT_EDGE, /* wait for the next falling edge (sequence check; *wait_us is the time to it), then step again */
	CO5300_TE_FRAME_REPLAN,    /* order[k] missed its window: plan order[k..] for the scans after the target */
	CO5300_TE_FRAME_LOST,      /* timing lost: order[k..] back to back, the edge rule only if k == 0 */
};

struct co5300_te_frame_run {  /* the plan being followed; caller-owned, writer only */
	int64_t ref_us;            /* E_ref: the plan's falling edge, driver us clock */
	int32_t scan;              /* target scan relative to ref_us */
	uint32_t epoch;
	uint8_t k;                 /* next position in order[] */
	uint8_t rounds;            /* wait rounds spent on order[k] */
	uint8_t replans;
};

struct co5300_te_frame_snap {  /* one TE reading, driver us clock */
	int64_t now_us;
	int64_t fall_us;           /* the last falling edge; the driver stamps it in the ISR and never later than the previous edge's stamp + S */
	uint32_t s_us;
	uint32_t tvdl_us;
	uint32_t epoch;
	bool valid;                /* estimates valid, edge fresh, not blanked, window on, normal orientation */
};

/*
 * The target scan relative to the newest edge: the plan's scan minus the edges that came since E_ref,
 * counted by time (rounded to the nearest), not by the edge count, which a lost edge makes short. A
 * REPLAN plans the rest with min_scan = this + 1 (and the frame's own bound minus the same edges).
 */
static inline int32_t co5300_te_frame_rel_scan(const struct co5300_te_frame_run *run, int64_t fall_us,
					       uint32_t s_us) {
	if (s_us == 0) {
		return run->scan;
	}
	return (int32_t)((int64_t)run->scan -
			 co5300_te_fdiv(fall_us - run->ref_us + (int64_t)s_us / 2, (int64_t)s_us));
}

static inline enum co5300_te_frame_action co5300_te_frame_step(const struct co5300_te_frame_run *run,
							       const int64_t *a_us, const int64_t *b_us,
							       const uint8_t *order,
							       const struct co5300_te_frame_snap *snap,
							       uint32_t *wait_us) {
	*wait_us = 0;
	if (!snap->valid || snap->epoch != run->epoch || snap->s_us == 0) {
		return CO5300_TE_FRAME_LOST;
	}

	const int64_t s = snap->s_us;
	/*
	 * An edge stamp can only be late (the interrupt ran late), never early, while S drifts by microseconds over
	 * the few scans of a plan. So the newest edge is taken no later than E_ref plus the edges between, which
	 * keeps one late interrupt from making the age look short and a rect start after its window.
	 */
	int64_t fall = snap->fall_us;
	int64_t edges = co5300_te_fdiv(fall - run->ref_us + s / 2, s);

	if (edges >= 0 && fall > run->ref_us + edges * s) {
		fall = run->ref_us + edges * s;
	}
	int64_t age = snap->now_us - fall;

	if (age < 0) {
		age = 0;
	}
	if (age > s + s / 10) {
		return CO5300_TE_FRAME_LOST;
	}

	uint8_t i = order[run->k];
	int64_t lo;
	int64_t hi;

	co5300_te_frame_window(a_us[i], b_us[i], snap->s_us, co5300_te_frame_rel_scan(run, fall, snap->s_us),
			       &lo, &hi);
	if (age >= hi) {
		return run->replans >= CO5300_TE_FRAME_MAX_REPLANS ? CO5300_TE_FRAME_LOST : CO5300_TE_FRAME_REPLAN;
	}
	if (age >= lo) {
		return CO5300_TE_FRAME_START;
	}
	if (run->rounds >= CO5300_TE_FRAME_MAX_WAITS) {
		return run->replans >= CO5300_TE_FRAME_MAX_REPLANS ? CO5300_TE_FRAME_LOST : CO5300_TE_FRAME_REPLAN;
	}
	/* As the decide: an age past one scan means the newest edge is late, so the phase is taken modulo S. */
	if (age % s + (lo - age) >= s) {
		*wait_us = (uint32_t)(s - age % s);
		return CO5300_TE_FRAME_WAIT_EDGE;
	}
	*wait_us = (uint32_t)(lo - age);
	return CO5300_TE_FRAME_WAIT;
}

/*
 * The D range of a frame: D = the time a rect's first or last row was written minus the time scan 0 (at
 * ref) reads it. Both ends of every rect; D is linear in the row for a straight-line writer.
 */
static inline void co5300_te_frame_span(uint8_t n, const uint16_t *y0, const uint16_t *y1, const int64_t *start_us,
					const int64_t *end_us, int64_t ref_us, uint32_t tvdl_us, uint16_t panel_h,
					int64_t *lo, int64_t *hi) {
	for (uint8_t i = 0; i < n; i++) {
		int64_t d0 = start_us[i] - ref_us - (int64_t)co5300_te_row_time(tvdl_us, panel_h, y0[i]);
		int64_t d1 = end_us[i] - ref_us - (int64_t)co5300_te_row_time(tvdl_us, panel_h, y1[i]);

		if (i == 0 || d0 < *lo) {
			*lo = d0;
		}
		if (i == 0 || d0 > *hi) {
			*hi = d0;
		}
		if (d1 < *lo) {
			*lo = d1;
		}
		if (d1 > *hi) {
			*hi = d1;
		}
	}
}

/* Section 5.8: the bound a frame leaves, from its stamps, and N_min for the next frame. */
struct co5300_te_frame_bound {
	int64_t at_us;   /* the start of the earliest scan allowed to show the next frame */
	uint32_t epoch;
	bool valid;
	bool scan_start; /* at_us is a scan start (round to the nearest scan); otherwise an end time (round up) */
};

/*
 * With timing (timed): the start of the first scan that showed all of the frame, E_ref + (floor((hi + m) / S) +
 * 1) * S; the margin keeps an interior excursion near a scan boundary from making it one scan early.
 * Without: the end of the last rect, since any scan that starts later shows all of it.
 */
static inline struct co5300_te_frame_bound
co5300_te_frame_finish(uint8_t n, const uint16_t *y0, const uint16_t *y1, const int64_t *start_us,
		       const int64_t *end_us, int64_t ref_us, uint32_t s_us, uint32_t tvdl_us, uint16_t panel_h,
		       uint32_t margin_us, bool timed, uint32_t epoch) {
	struct co5300_te_frame_bound b = {0, epoch, false, false};

	if (n == 0) {
		return b;
	}
	b.valid = true;
	if (timed && s_us != 0) {
		int64_t lo = 0;
		int64_t hi = 0;

		co5300_te_frame_span(n, y0, y1, start_us, end_us, ref_us, tvdl_us, panel_h, &lo, &hi);
		b.at_us = ref_us + (co5300_te_fdiv(hi + (int64_t)margin_us, s_us) + 1) * (int64_t)s_us;
		b.scan_start = true;
		return b;
	}
	b.at_us = end_us[0];
	for (uint8_t i = 1; i < n; i++) {
		if (end_us[i] > b.at_us) {
			b.at_us = end_us[i];
		}
	}
	return b;
}

/*
 * Note: finish() of a frame that wrote no rect (n == 0) returns an invalid bound; the caller then keeps the
 * previous frame's bound (only a new epoch voids it), since that frame's scans are still pending.
 */

/* N_min for the next frame, relative to its snapshot's edge fall_us; 0 when the bound is void or from another epoch. */
static inline int32_t co5300_te_frame_min_scan(const struct co5300_te_frame_bound *bound, int64_t fall_us,
					       uint32_t s_us, uint32_t epoch) {
	if (bound == NULL || !bound->valid || bound->epoch != epoch || s_us == 0) {
		return 0;
	}

	const int64_t s = s_us;
	int64_t d = bound->at_us - fall_us;
	/* An end time: the first scan that starts after it */
	int64_t n = bound->scan_start ? co5300_te_fdiv(d + s / 2, s) : co5300_te_fdiv(d, s) + 1;

	if (n < 0) {
		return 0;
	}
	return n > INT32_MAX ? INT32_MAX : (int32_t)n;
}

struct co5300_te_frame_verdict {
	bool mixed;        /* some scan showed part of the frame new and part old */
	bool late;         /* some rect took longer than w_max + margin */
	int64_t shown_us;  /* the start of the first scan that showed all of it */
};

/*
 * Closed-form audit from measured stamps (section 5.10), O(n). A scan boundary inside (lo, hi] of the D
 * range means a scan saw some rects new and others old; ties count as old, as in co5300_te_audit_torn().
 */
static inline struct co5300_te_frame_verdict
co5300_te_frame_audit(uint8_t n, const uint16_t *y0, const uint16_t *y1, const int64_t *start_us,
		      const int64_t *end_us, const uint32_t *w_max_us, int64_t ref_us, uint32_t s_us,
		      uint32_t tvdl_us, uint16_t panel_h, uint32_t margin_us) {
	struct co5300_te_frame_verdict v = {false, false, 0};

	if (n == 0 || s_us == 0) {
		return v;
	}

	int64_t lo = 0;
	int64_t hi = 0;

	co5300_te_frame_span(n, y0, y1, start_us, end_us, ref_us, tvdl_us, panel_h, &lo, &hi);
	v.mixed = co5300_te_fdiv(lo, s_us) != co5300_te_fdiv(hi, s_us);
	v.shown_us = ref_us + (co5300_te_fdiv(hi, s_us) + 1) * (int64_t)s_us;
	for (uint8_t i = 0; i < n; i++) {
		if (end_us[i] - start_us[i] > (int64_t)w_max_us[i] + (int64_t)margin_us) {
			v.late = true;
		}
	}
	return v;
}

#ifdef __cplusplus
}
#endif
