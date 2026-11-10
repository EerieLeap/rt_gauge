/*
 * Copyright (c) 2025 Pavel Maloletkov.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The frame planner and its run-time steps (FRAME_CONSISTENT_WRITES_PLAN.md, sections 5 and 6.3), against
 * a frozen copy of the single-rect rule, a brute force over every order and scan, and a simulation of
 * the writer that checks every row of every rect against the panel's scans.
 */

#include <string.h>
#include <zephyr/ztest.h>

#include "co5300_te_window.h"

/* The numbers measured on the unit (2026-11-09), as in te_window_test.c. */
enum {
    S_US = 16800,
    TVDL_US = 16190,
    H = 466,
    MARGIN = 500,
    GAP = CO5300_TE_FRAME_GAP_US,
    MAXR = 64,
    MAXP = 16,
    PIECE_BYTES = 32768,
    NS_PER_BYTE = 50,
};

static int64_t row_t(uint16_t y) {
    return co5300_te_row_time(TVDL_US, H, y);
}

/* ---- Random numbers: xorshift32 for the frames, a hash for what belongs to an edge ---- */

static uint32_t rng_state = 1;

static void seed_rng(uint32_t seed) {
    rng_state = seed != 0 ? seed : 1U;
}

static uint32_t rnd_u32(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

/* Inclusive range. */
static int64_t rnd_in(int64_t lo, int64_t hi) {
    return hi <= lo ? lo : lo + (int64_t)(rnd_u32() % (uint64_t)(hi - lo + 1));
}

static uint64_t mix_bits(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

static int64_t hash_rand(uint64_t salt, int64_t k, int64_t max) {
    return max <= 0 ? 0 : (int64_t)(mix_bits((salt << 32) ^ (uint64_t)k) % (uint64_t)(max + 1));
}

/* ---- Frames of rects ---- */

struct Frame {
    uint8_t n;
    uint16_t y0[MAXR];
    uint16_t y1[MAXR];
    uint16_t x[MAXR]; /* width in pixels, 2 bytes each */
    uint32_t wmin[MAXR];
    uint32_t wmax[MAXR];
};

static uint32_t rect_bytes(const struct Frame* f, uint8_t i) {
    return (uint32_t)f->x[i] * (uint32_t)(f->y1[i] - f->y0[i] + 1) * 2U;
}

static void add_rect(struct Frame* f, uint16_t y0, uint16_t y1, uint16_t width) {
    f->y0[f->n] = y0;
    f->y1[f->n] = y1;
    f->x[f->n] = width;
    f->n++;
}

/* The write-time bounds around the model: [W - lo_spread, W + hi_spread]. */
static void set_bounds(struct Frame* f, uint32_t lo_spread, uint32_t hi_spread) {
    for(uint8_t i = 0; i < f->n; i++) {
        uint32_t w = co5300_te_w_model_us(rect_bytes(f, i), NS_PER_BYTE);

        f->wmin[i] = w > lo_spread + 1U ? w - lo_spread : 1U;
        f->wmax[i] = w + hi_spread;
    }
}

/* The drag test's envelope: class 2 (128 KiB and more) is -130/+1021, the others the plan's -100/+300. */
static void set_bounds_drag(struct Frame* f) {
    set_bounds(f, 100, 300);
    for(uint8_t i = 0; i < f->n; i++) {
        if(co5300_te_w_class(rect_bytes(f, i)) == 2U) {
            uint32_t w = co5300_te_w_model_us(rect_bytes(f, i), NS_PER_BYTE);

            f->wmin[i] = w - 130U;
            f->wmax[i] = w + 1021U;
        }
    }
}

/* The five rects of the drag frame (section 5.12), in LVGL's order: the needle's box first. */
enum {
    DRAG_NEEDLE,
    DRAG_CHART,
    DRAG_BAR,
    DRAG_NUMBER,
    DRAG_SLIDER
};

static void drag_frame(struct Frame* f) {
    *f = (struct Frame){ 0 };
    add_rect(f, 19, 238, 120);
    add_rect(f, 32, 185, 466);
    add_rect(f, 245, 274, 466);
    add_rect(f, 260, 356, 172);
    add_rect(f, 360, 423, 230);
}

static void random_rect(struct Frame* f) {
    uint32_t kind = rnd_u32() % 100;

    if(kind < 7) {
        /* narrow and full height */
        add_rect(f, 0, H - 1, (uint16_t)rnd_in(1, 60));
        return;
    }

    uint16_t h = (uint16_t)rnd_in(1, 240);
    uint16_t y0 = (uint16_t)rnd_in(0, H - h);
    uint16_t w = rnd_u32() % 100 < 35 ? (uint16_t)H : (uint16_t)rnd_in(1, H);

    add_rect(f, y0, (uint16_t)(y0 + h - 1), w);
}

static void random_frame(struct Frame* f, unsigned min_n, unsigned max_n) {
    *f = (struct Frame){ 0 };

    unsigned n = (unsigned)rnd_in(min_n, max_n);

    for(unsigned i = 0; i < n; i++) {
        random_rect(f);
    }
}

static void copy_bytes(uint8_t* dst, const uint8_t* src, size_t n) {
    for(size_t i = 0; i < n; i++) {
        dst[i] = src[i];
    }
}

/* ---- The planner, called the way the driver does ---- */

struct PlanBuf {
    int64_t a[MAXR];
    int64_t b[MAXR];
    uint8_t best[MAXR];
    uint8_t stack[CO5300_TE_FRAME_SEARCH_MAX];
    int64_t st[CO5300_TE_FRAME_SEARCH_MAX];
    uint8_t order[MAXR];
    int32_t scan;
    uint32_t nodes;
};

static enum co5300_te_frame_result plan_frame(
    struct PlanBuf* pb,
    const struct Frame* f,
    uint32_t age,
    int32_t min_scan,
    uint32_t margin,
    uint32_t gap,
    bool valid
) {
    struct co5300_te_frame_in in = {
        .age_us = age,
        .s_us = S_US,
        .tvdl_us = TVDL_US,
        .panel_h = H,
        .margin_us = margin,
        .gap_us = gap,
        .min_scan = min_scan,
        .valid = valid,
        .n = f->n,
        .y0 = f->y0,
        .y1 = f->y1,
        .w_min_us = f->wmin,
        .w_max_us = f->wmax,
    };
    struct co5300_te_frame_scratch sc = {
        .a_us = pb->a, .b_us = pb->b, .best = pb->best, .stack = pb->stack, .stack_t_us = pb->st
    };

    pb->scan = -1;

    enum co5300_te_frame_result r = co5300_te_frame_plan(&in, &sc, pb->order, &pb->scan, &pb->nodes);

    /* The search budget is per frame, over every scan tried: every caller checks it here */
    zassert_true(pb->nodes <= CO5300_TE_FRAME_SEARCH_NODES, "%u search nodes", pb->nodes);
    return r;
}

/* ---- An independent reference: bounds, schedule and brute force over orders and scans ---- */

static void ref_bounds(const struct Frame* f, uint8_t i, uint32_t margin, int64_t* a, int64_t* b) {
    int64_t t0 = row_t(f->y0[i]);
    int64_t t1 = row_t(f->y1[i]);
    int64_t lo = t1 - (int64_t)f->wmax[i];
    int64_t hi = t1 - (int64_t)f->wmin[i];

    *a = (lo < t0 ? lo : t0) - (int64_t)margin;
    *b = (hi > t0 ? hi : t0) + (int64_t)margin;
}

/* The earliest-start schedule of an order for scan n: feasible when every start is before its U. */
static bool ref_follow(
    const struct Frame* f,
    const uint8_t* order,
    int64_t age,
    int32_t n,
    uint32_t margin,
    uint32_t gap,
    int64_t* first,
    int64_t* end
) {
    int64_t t = age;

    for(uint8_t k = 0; k < f->n; k++) {
        int64_t a = 0;
        int64_t b = 0;

        ref_bounds(f, order[k], margin, &a, &b);

        int64_t lo = ((int64_t)n - 1) * S_US + b;
        int64_t hi = (int64_t)n * S_US + a;
        int64_t start = t > lo ? t : lo;

        if(start >= hi) {
            return false;
        }
        if(k == 0) {
            *first = start;
        }
        *end = start + f->wmax[order[k]];
        t = *end + gap;
    }
    return true;
}

static bool next_perm(uint8_t* p, uint8_t n) {
    int i = (int)n - 2;

    while(i >= 0 && p[i] >= p[i + 1]) {
        i--;
    }
    if(i < 0) {
        return false;
    }

    int j = (int)n - 1;

    while(p[j] <= p[i]) {
        j--;
    }

    uint8_t tmp = p[i];

    p[i] = p[j];
    p[j] = tmp;
    for(int l = i + 1, r = (int)n - 1; l < r; l++, r--) {
        tmp = p[l];
        p[l] = p[r];
        p[r] = tmp;
    }
    return true;
}

struct BruteResult {
    bool found;
    int32_t scan;
    int64_t end; /* the smallest end at that scan over every order */
};

/* Over every order (n <= 6) or the sorted order only, and the scans min_scan .. min_scan + 5. */
static struct BruteResult
brute_force(const struct Frame* f, int64_t age, int32_t min_scan, uint32_t margin, uint32_t gap, bool all_orders) {
    struct BruteResult r = { false, 0, 0 };
    uint8_t order[MAXR];

    for(int32_t n = min_scan; n < min_scan + 6; n++) {
        for(uint8_t i = 0; i < f->n; i++) {
            order[i] = i;
        }
        if(!all_orders) {
            /* by b, then y0, then index */
            for(uint8_t i = 1; i < f->n; i++) {
                uint8_t v = order[i];
                int64_t av = 0;
                int64_t bv = 0;
                uint8_t j = i;

                ref_bounds(f, v, margin, &av, &bv);
                while(j > 0) {
                    int64_t ap = 0;
                    int64_t bp = 0;

                    ref_bounds(f, order[j - 1], margin, &ap, &bp);
                    if(!(bv < bp || (bv == bp && f->y0[v] < f->y0[order[j - 1]]))) {
                        break;
                    }
                    order[j] = order[j - 1];
                    j--;
                }
                order[j] = v;
            }
        }
        do {
            int64_t first = 0;
            int64_t end = 0;

            if(ref_follow(f, order, age, n, margin, gap, &first, &end) && (!r.found || end < r.end)) {
                r.found = true;
                r.scan = n;
                r.end = end;
            }
        } while(all_orders && next_perm(order, f->n));
        if(r.found) {
            return r;
        }
    }
    return r;
}

static bool is_permutation(const uint8_t* order, uint8_t n) {
    uint64_t seen = 0;

    for(uint8_t i = 0; i < n; i++) {
        if(order[i] >= n || ((seen >> order[i]) & 1U) != 0U) {
            return false;
        }
        seen |= 1ULL << order[i];
    }
    return true;
}

/* ---- A frozen copy of today's decide (before the bounds were factored out) ---- */

static struct co5300_te_decision frozen_decide(const struct co5300_te_window_in* in) {
    struct co5300_te_decision out = { CO5300_TE_FALLBACK, 0 };

    if(!in->valid
        || in->s_us == 0
        || in->tvdl_us == 0
        || in->tvdl_us > in->s_us
        || in->panel_h == 0
        || in->y0 > in->y1
        || in->y1 >= in->panel_h
        || in->w_min_us > in->w_max_us) {
        return out;
    }
    if((uint64_t)in->age_us > (uint64_t)in->s_us + in->s_us / 10) {
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

    if(width >= s) {
        return out;
    }

    int64_t phase = (int64_t)in->age_us % s;
    int64_t d = co5300_te_mod(phase - a, s);

    if(d >= width) {
        out.action = CO5300_TE_START_NOW;
        return out;
    }

    int64_t wait = width - d;

    if(phase + wait >= s) {
        out.action = CO5300_TE_WAIT_EDGE;
        out.wait_us = (uint32_t)(s - phase);
    } else {
        out.action = CO5300_TE_WAIT;
        out.wait_us = (uint32_t)wait;
    }
    return out;
}

/* One random single-area input: the sweep of the first two tests. */
static struct co5300_te_window_in random_window_in(void) {
    struct co5300_te_window_in in = {
        .s_us = S_US,
        .tvdl_us = TVDL_US,
        .panel_h = H,
        .valid = true,
    };
    uint16_t a = (uint16_t)rnd_in(0, H - 1);
    uint16_t b = (uint16_t)rnd_in(0, H - 1);

    in.y0 = a < b ? a : b;
    in.y1 = a < b ? b : a;
    in.w_min_us = (uint32_t)rnd_in(1, 30000);
    in.w_max_us = in.w_min_us + (uint32_t)rnd_in(0, 1500);
    in.margin_us = (uint32_t)rnd_in(0, 2000);
    return in;
}

ZTEST(te_frame, test_decide_matches_frozen_copy) {
    seed_rng(0x1234abcd);

    unsigned compared = 0;

    for(unsigned c = 0; c < 6000; c++) {
        struct co5300_te_window_in in = random_window_in();

        if(c % 7 == 0) {
            /* other timings, now and then */
            in.s_us = (uint32_t)rnd_in(5000, 60000);
            in.tvdl_us = (uint32_t)rnd_in(1, in.s_us + 100);
        }
        if(c % 31 == 0) {
            in.valid = false;
        }
        if(c % 53 == 0) {
            in.y1 = (uint16_t)(in.y1 + 400U);
        }
        for(uint32_t age = 0; age <= in.s_us + in.s_us / 10 + 50; age += 97) {
            in.age_us = age;

            struct co5300_te_decision want = frozen_decide(&in);
            struct co5300_te_decision got = co5300_te_window_decide(&in);

            if(want.action != got.action || want.wait_us != got.wait_us) {
                zassert_true(
                    false, "case %u age %u: action %d/%d wait %u/%u", c, age, want.action, got.action, want.wait_us,
                    got.wait_us
                );
            }
            compared++;
        }
    }
    zassert_true(compared > 500000);
}

ZTEST(te_frame, test_window_bounds_are_the_decides_interval) {
    int64_t a = 0;
    int64_t b = 0;

    /* The full frame of te_window_test.c: a = -8245, b = 500 */
    co5300_te_window_bounds(TVDL_US, H, 0, 465, 23500, 23900, MARGIN, &a, &b);
    zassert_equal(a, row_t(465) - 23900 - MARGIN);
    zassert_equal(b, MARGIN);

    /* A small area: the writer is faster than the scan, a = t0 - m, b = t1 - wmin + m */
    co5300_te_window_bounds(TVDL_US, H, 100, 200, 1000, 1200, 0, &a, &b);
    zassert_equal(a, row_t(100));
    zassert_equal(b, row_t(200) - 1000);

    int64_t lo = 0;
    int64_t hi = 0;

    co5300_te_frame_window(a, b, S_US, 2, &lo, &hi);
    zassert_equal(lo, S_US + b);
    zassert_equal(hi, 2LL * S_US + a);
}

ZTEST(te_frame, test_single_rect_is_decide) {
    seed_rng(0x5eed0002);

    unsigned compared = 0;

    for(unsigned c = 0; c < 3000; c++) {
        struct co5300_te_window_in in = random_window_in();
        struct Frame f;
        struct PlanBuf pb;

        f = (struct Frame){ 0 };
        add_rect(&f, in.y0, in.y1, 10);
        f.wmin[0] = in.w_min_us;
        f.wmax[0] = in.w_max_us;
        if(c % 11 == 0) {
            in.margin_us = (uint32_t)rnd_in(2000, 9000); /* a rect without a window of its own */
        }
        for(uint32_t age = 0; age <= S_US + S_US / 10 + 50; age += 83) {
            in.age_us = age;

            struct co5300_te_decision d = co5300_te_window_decide(&in);
            enum co5300_te_frame_result r = plan_frame(&pb, &f, age, 0, in.margin_us, GAP, true);

            if(d.action == CO5300_TE_FALLBACK) {
                zassert_true(r != CO5300_TE_FRAME_OK, "case %u age %u: decide falls back, plan is OK", c, age);
                compared++;
                continue;
            }
            zassert_equal(r, CO5300_TE_FRAME_OK, "case %u age %u: plan %d", c, age, r);

            struct co5300_te_frame_run run = { .ref_us = 0, .scan = pb.scan, .epoch = 1, .k = 0 };
            struct co5300_te_frame_snap snap = {
                .now_us = age, .fall_us = 0, .s_us = S_US, .tvdl_us = TVDL_US, .epoch = 1, .valid = true
            };
            uint32_t wait = 0;
            enum co5300_te_frame_action act = co5300_te_frame_step(&run, pb.a, pb.b, pb.order, &snap, &wait);

            switch(d.action) {
                case CO5300_TE_START_NOW:
                    zassert_equal(act, CO5300_TE_FRAME_START, "case %u age %u scan %d: act %d", c, age, pb.scan, act);
                    break;
                case CO5300_TE_WAIT:
                    zassert_equal(act, CO5300_TE_FRAME_WAIT, "case %u age %u: act %d", c, age, act);
                    zassert_equal(wait, d.wait_us, "case %u age %u: wait %u/%u", c, age, wait, d.wait_us);
                    break;
                default:
                    zassert_equal(act, CO5300_TE_FRAME_WAIT_EDGE, "case %u age %u: act %d", c, age, act);
                    zassert_equal(wait, d.wait_us, "case %u age %u: wait %u/%u", c, age, wait, d.wait_us);
                    break;
            }
            compared++;
        }
    }
    zassert_true(compared > 200000);
}

/* ---- The writer simulation ---- */

/* How a frame's bound is kept: the real one, the planned scan (the first version), or none. */
enum BoundPolicy {
    POL_STAMPS,
    POL_PLANNED,
    POL_NONE
};

struct SimConfig {
    int64_t lat_max;       /* an edge is stamped this late (the ISR) */
    int64_t late_max;      /* and delivered this much later still (a late interrupt) */
    int64_t delay_max;     /* the first pixel follows the start by this much */
    int64_t pause_max;     /* pause after every 32 KB piece */
    int64_t drift;         /* the true scan period differs from the estimate by up to this */
    int64_t jitter;        /* a sleep wakes up to this late */
    int64_t gap_max;       /* time from a rect's end to the next step */
    int64_t stall_between; /* up to this much extra between two rects, now and then */
    int64_t stall_inside;  /* a rect takes up to this much longer than w_max, now and then (above the margin) */
    int64_t stall_once;    /* one stall after the first rect of the first frame */
    int64_t lose_from;     /* every edge from this one on is lost; -1 for none */
    int lose_pct;
    int stall_between_pct;
    int stall_inside_pct;
    int early_wake_pct; /* a sleep wakes after a third of the wait */
    int eio_pct;
    int invalid_start_pct; /* the first snapshot of a frame is not valid */
    int blank_step;        /* the epoch changes at this step; -1 for never */
    int off_step;          /* the window rule goes off at this step; -1 for never */
    int wmode;             /* 0: w_min, 1: w_max, 2: anything between */
    enum BoundPolicy policy;
};

struct FrameOut {
    int64_t s[MAXR];
    int64_t e[MAXR];
    int64_t delay[MAXR];
    int32_t pause[MAXR][MAXP];
    uint8_t attempts[MAXR];
    bool done[MAXR];
    bool failed[MAXR];
    uint8_t nw;
    uint8_t nfailed;
    int64_t ref;
    int32_t planned;
    bool have_plan;
    bool lost;
    bool fallback;
    bool nowin;
    uint8_t replans;
    uint8_t misses;
    uint32_t epoch0;
    uint32_t epoch1;
};

struct SimState {
    struct SimConfig c;
    uint64_t salt;
    int64_t st; /* the true scan period */
    int64_t t;  /* now, on the true clock: scan k starts at k * st */
    uint32_t epoch;
    int steps;
    int64_t late_edge1; /* edge 1 is delivered no earlier than this */
    int64_t tol;        /* rows are checked with this much rounding slack (the plan's row times are floors) */
    bool once_pending;
    struct co5300_te_frame_bound bound;
};

static void sim_init(struct SimState* sm, const struct SimConfig* c, uint64_t salt) {
    *sm = (struct SimState){ 0 };
    sm->c = *c;
    sm->salt = salt;
    sm->st = S_US + hash_rand(salt + 99, 0, 2 * c->drift) - c->drift;
    sm->epoch = 1;
    sm->once_pending = c->stall_once > 0;
}

static int64_t edge_stamp(const struct SimState* sm, int64_t k) {
    /* The driver stamps in the ISR, so a late interrupt is a late stamp. */
    return k * sm->st + hash_rand(sm->salt + 1, k, sm->c.lat_max) + hash_rand(sm->salt + 2, k, sm->c.late_max);
}

static bool edge_lost(const struct SimState* sm, int64_t k);

/*
 * What the driver keeps as the edge's time: the ISR stamp, but never later than an earlier edge's stamp plus
 * the scans between (a stamp can only be late). Up to three edges back, skipping lost ones.
 */
static int64_t edge_driver_stamp(const struct SimState* sm, int64_t k) {
    int64_t t = edge_stamp(sm, k);

    for(int64_t j = 1; j <= 3 && k - j >= 0; j++) {
        if(!edge_lost(sm, k - j)) {
            int64_t c = edge_stamp(sm, k - j) + j * (int64_t)S_US;

            if(c < t) {
                t = c;
            }
        }
    }
    return t;
}

static int64_t edge_deliver(const struct SimState* sm, int64_t k) {
    int64_t d = edge_stamp(sm, k);

    if(k == 1 && d < sm->late_edge1) {
        d = sm->late_edge1;
    }
    return d;
}

static bool edge_lost(const struct SimState* sm, int64_t k) {
    if(k < 1) {
        return false;
    }
    if(sm->c.lose_from >= 0 && k >= sm->c.lose_from) {
        return true;
    }
    if(sm->c.lose_pct <= 0) {
        return false;
    }
    return hash_rand(sm->salt + 3, k, 99) < sm->c.lose_pct;
}

static struct co5300_te_frame_snap take_snapshot(struct SimState* sm) {
    struct co5300_te_frame_snap sn = { .now_us = sm->t, .s_us = S_US, .tvdl_us = TVDL_US, .epoch = 0, .valid = false };

    sm->steps++;
    if(sm->steps == sm->c.blank_step) {
        sm->epoch++;
    }
    sn.epoch = sm->epoch;

    int64_t k = sm->t / sm->st;

    for(int i = 0; i < 8 && k >= 0 && (edge_lost(sm, k) || edge_deliver(sm, k) > sm->t); i++) {
        k--;
    }
    if(k >= 0 && !edge_lost(sm, k) && edge_deliver(sm, k) <= sm->t) {
        sn.fall_us = edge_driver_stamp(sm, k);
        sn.valid = true;
        if(sm->c.off_step >= 0 && sm->steps >= sm->c.off_step) {
            sn.valid = false;
        }
    }
    return sn;
}

/* Sleeps until the next edge that is delivered (the driver waits on its semaphore), at most 1.2 scans. */
static void wait_next_edge(struct SimState* sm) {
    int64_t k = sm->t / sm->st;

    for(int64_t i = k; i <= k + 4; i++) {
        if(!edge_lost(sm, i) && edge_deliver(sm, i) > sm->t) {
            if(edge_deliver(sm, i) - sm->t <= sm->st + sm->st / 5) {
                sm->t = edge_deliver(sm, i);
                return;
            }
            break;
        }
    }
    sm->t += sm->st + sm->st / 5;
}

/* The time a rect takes: inside its bounds, plus the odd stall above them. */
static int64_t pick_w(struct SimState* sm, const struct Frame* f, uint8_t i) {
    int64_t w = f->wmin[i];

    if(sm->c.wmode == 1) {
        w = f->wmax[i];
    } else if(sm->c.wmode == 2) {
        w = rnd_in(f->wmin[i], f->wmax[i]);
    }
    if(sm->c.stall_inside_pct > 0 && (int)(rnd_u32() % 100) < sm->c.stall_inside_pct) {
        w = f->wmax[i] + rnd_in(MARGIN + 100, sm->c.stall_inside);
    }
    return w;
}

/* The driver sends a rect three times at most (the first send and two re-sends). */
enum {
    MAX_ATTEMPTS = 3
};

/* One attempt to write rect i. False: it failed (-EIO) and the caller must send it again. */
static bool write_rect(struct SimState* sm, const struct Frame* f, uint8_t i, struct FrameOut* o) {
    int64_t w = pick_w(sm, f, i);

    if(sm->c.eio_pct > 0 && o->attempts[i] < MAX_ATTEMPTS && (int)(rnd_u32() % 100) < sm->c.eio_pct) {
        o->attempts[i]++;
        sm->t += w * rnd_in(10, 90) / 100;
        return false;
    }

    uint32_t pieces = (rect_bytes(f, i) + PIECE_BYTES - 1U) / PIECE_BYTES;

    o->s[i] = sm->t;
    o->e[i] = sm->t + w;
    o->delay[i] = rnd_in(0, sm->c.delay_max);
    for(uint32_t p = 0; p + 1U < pieces && p < MAXP; p++) {
        o->pause[i][p] = (int32_t)rnd_in(0, sm->c.pause_max);
    }
    o->done[i] = true;
    o->nw++;

    sm->t = o->e[i] + rnd_in(0, sm->c.gap_max);
    if(sm->c.stall_between_pct > 0 && (int)(rnd_u32() % 100) < sm->c.stall_between_pct) {
        sm->t += rnd_in(1, sm->c.stall_between);
    }
    if(sm->once_pending) {
        sm->once_pending = false;
        sm->t += sm->c.stall_once;
    }
    return true;
}

/* Sends rect i until it is written or has failed every attempt (no timing to follow: the lost path). */
static void write_until_done(struct SimState* sm, const struct Frame* f, uint8_t i, struct FrameOut* o) {
    while(!write_rect(sm, f, i, o)) {
        if(o->attempts[i] >= MAX_ATTEMPTS) {
            o->failed[i] = true;
            o->nfailed++;
            return;
        }
    }
}

/* Timing lost or never there: back to back in y order; the edge rule only when nothing was written yet. */
static void run_lost(struct SimState* sm, const struct Frame* f, const uint8_t* orig, uint8_t cnt, struct FrameOut* o) {
    uint8_t ord[MAXR];

    for(uint8_t i = 0; i < cnt; i++) {
        uint8_t j = i;

        while(j > 0 && f->y0[orig[ord[j - 1]]] > f->y0[orig[i]]) {
            ord[j] = ord[j - 1];
            j--;
        }
        ord[j] = i;
    }
    if(o->nw == 0) {
        wait_next_edge(sm);
    }
    for(uint8_t i = 0; i < cnt; i++) {
        write_until_done(sm, f, orig[ord[i]], o);
    }
}

static int32_t policy_min(const struct SimState* sm, const struct co5300_te_frame_snap* sn) {
    if(sm->c.policy == POL_NONE) {
        return 0;
    }
    return co5300_te_frame_min_scan(&sm->bound, sn->fall_us, sn->s_us, sn->epoch);
}

/*
 * The writer's loop for one frame, as the driver will run it: plan, then step before every rect, and
 * act on the answer. Re-plans and split frames are loops over a list of pending rects, not recursion.
 */
// NOLINTNEXTLINE(readability-function-cognitive-complexity): it mirrors the driver's loop, whose branches are the point
static void sim_frame(struct SimState* sm, const struct Frame* f, struct FrameOut* o) {
    uint8_t pending[MAXR];
    uint8_t cnt = f->n;
    bool split = false;
    bool first_plan = true;
    struct co5300_te_frame_bound floor_b = { 0, 0, false, false };

    *o = (struct FrameOut){ 0 };
    for(uint8_t i = 0; i < f->n; i++) {
        pending[i] = i;
    }
    o->epoch0 = sm->epoch;

    while(cnt > 0) {
        uint8_t take = split ? 1 : cnt;
        struct Frame g;
        struct PlanBuf pb;

        g = (struct Frame){ 0 };
        for(uint8_t i = 0; i < take; i++) {
            uint8_t s = pending[i];

            add_rect(&g, f->y0[s], f->y1[s], f->x[s]);
            g.wmin[i] = f->wmin[s];
            g.wmax[i] = f->wmax[s];
        }

        struct co5300_te_frame_snap sn = take_snapshot(sm);

        if(first_plan && (int)(rnd_u32() % 100) < sm->c.invalid_start_pct) {
            sn.valid = false;
        }
        floor_b.epoch = sn.epoch;

        int32_t m1 = co5300_te_frame_min_scan(&floor_b, sn.fall_us, sn.s_us, sn.epoch);
        int32_t m2 = policy_min(sm, &sn);
        int32_t min_scan = m1 > m2 ? m1 : m2;
        enum co5300_te_frame_result res = plan_frame(
            &pb, &g, (uint32_t)(sn.now_us - sn.fall_us), min_scan, MARGIN, GAP, sn.valid
        );

        if(first_plan) {
            o->ref = sn.fall_us;
            o->have_plan = res == CO5300_TE_FRAME_OK;
            o->planned = pb.scan;
            first_plan = false;
        }
        if(res == CO5300_TE_FRAME_FALLBACK) {
            o->fallback = true;
            o->lost = true;
            run_lost(sm, f, pending, cnt, o);
            break;
        }
        if(res == CO5300_TE_FRAME_NO_WINDOW) {
            o->nowin = true;
            if(take > 1) {
                /* written rect by rect in the default order, each in a window of its own */
                uint8_t ord[MAXR];
                uint8_t copy[MAXR];
                struct co5300_te_frame_in in = { .n = g.n, .y0 = g.y0 };
                struct co5300_te_frame_scratch sc = { .a_us = pb.a, .b_us = pb.b };

                co5300_te_frame_sorted(&in, &sc, ord);
                for(uint8_t i = 0; i < take; i++) {
                    copy[i] = pending[ord[i]];
                }
                copy_bytes(pending, copy, take);
                split = true;
                continue;
            }
            /* a rect without a window of its own: the edge rule if it is the first, else at once */
            run_lost(sm, f, pending, 1, o);
            for(uint8_t i = 1; i < cnt; i++) {
                pending[i - 1U] = pending[i];
            }
            cnt--;
            continue;
        }

        struct co5300_te_frame_run run = {
            .ref_us = sn.fall_us, .scan = pb.scan, .epoch = sn.epoch, .k = 0, .rounds = 0, .replans = o->replans
        };
        bool replanned = false;
        bool lost = false;

        while(run.k < take) {
            uint32_t wait = 0;

            sn = take_snapshot(sm);

            enum co5300_te_frame_action act = co5300_te_frame_step(&run, pb.a, pb.b, pb.order, &sn, &wait);

            if(act == CO5300_TE_FRAME_START) {
                uint8_t i = pending[pb.order[run.k]];

                if(!o->done[i] && !o->failed[i]) {
                    if(write_rect(sm, f, i, o)) {
                        run.k++;
                        run.rounds = 0;
                    } else if(o->attempts[i] >= MAX_ATTEMPTS) {
                        /* every attempt failed: reported, the rect is dropped (D-10) */
                        o->failed[i] = true;
                        o->nfailed++;
                        run.k++;
                        run.rounds = 0;
                    }
                    /* otherwise the next pass steps again before the re-send */
                }
            } else if(act == CO5300_TE_FRAME_WAIT) {
                sm->t += (sm->c.early_wake_pct > 0 && (int)(rnd_u32() % 100) < sm->c.early_wake_pct) ? wait / 3U : wait;
                sm->t += rnd_in(0, sm->c.jitter);
                run.rounds++;
            } else if(act == CO5300_TE_FRAME_WAIT_EDGE) {
                wait_next_edge(sm);
                run.rounds++;
            } else if(act == CO5300_TE_FRAME_REPLAN) {
                replanned = true;
                break;
            } else {
                lost = true;
                break;
            }
        }

        /* Drop what was written from the pending list, in plan order for the rest. */
        uint8_t rest[MAXR];
        uint8_t nrest = 0;

        for(uint8_t k = run.k; k < take; k++) {
            rest[nrest++] = pending[pb.order[k]];
        }
        for(uint8_t i = take; i < cnt; i++) {
            rest[nrest++] = pending[i];
        }
        copy_bytes(pending, rest, nrest);
        cnt = nrest;

        if(lost) {
            o->lost = true;
            run_lost(sm, f, pending, cnt, o);
            break;
        }
        if(replanned) {
            o->replans++;
            o->misses++;
            floor_b.at_us = run.ref_us + ((int64_t)run.scan + 1) * S_US;
            floor_b.valid = true;
            floor_b.scan_start = true;
        }
    }
    o->epoch1 = sm->epoch;
}

struct DRange {
    bool any;
    int64_t lo;
    int64_t hi;
};

static void drange_add(struct DRange* r, int64_t d) {
    if(!r->any || d < r->lo) {
        r->lo = d;
    }
    if(!r->any || d > r->hi) {
        r->hi = d;
    }
    r->any = true;
}

/* Every row of one written rect: the time it was written minus the time scan 0 reads it. */
static void rect_rows_range(const struct Frame* f, const struct FrameOut* o, uint8_t i, struct DRange* dr) {
    uint32_t rows = (uint32_t)(f->y1[i] - f->y0[i] + 1);
    uint32_t rb = (uint32_t)f->x[i] * 2U;
    uint32_t pieces = (rect_bytes(f, i) + PIECE_BYTES - 1U) / PIECE_BYTES;
    int64_t cum[MAXP + 1];
    int64_t total = 0;

    cum[0] = 0;
    for(uint32_t p = 0; p < MAXP; p++) {
        total += (p + 1U < pieces) ? o->pause[i][p] : 0;
        cum[p + 1] = total;
    }

    int64_t stream = (o->e[i] - o->s[i]) - o->delay[i] - total;

    if(stream < 0) {
        stream = 0;
    }
    if(rows == 1) {
        /* one row: written somewhere between the first pixel and the end */
        drange_add(dr, o->s[i] + o->delay[i] - row_t(f->y0[i]));
        drange_add(dr, o->e[i] - row_t(f->y0[i]));
        return;
    }
    for(uint32_t r = 0; r < rows; r++) {
        uint32_t piece = (uint32_t)(((uint64_t)(r + 1U) * rb - 1U) / PIECE_BYTES);
        int64_t tw = o->s[i] + o->delay[i] + stream * (int64_t)r / (int64_t)(rows - 1U)
            + cum[piece < MAXP ? piece : MAXP];

        drange_add(dr, tw - row_t((uint16_t)(f->y0[i] + r)));
    }
}

/* The D range of the written rects, row by row, against the true scans (scan k starts at k * st). */
static bool rows_range(const struct Frame* f, const struct FrameOut* o, int64_t* dmin, int64_t* dmax) {
    struct DRange dr = { false, 0, 0 };

    for(uint8_t i = 0; i < f->n; i++) {
        if(o->done[i]) {
            rect_rows_range(f, o, i, &dr);
        }
    }
    *dmin = dr.lo;
    *dmax = dr.hi;
    return dr.any;
}

/* What the panel showed: mixed, and the scan (counted from the true edge 0) that first showed all of it. */
struct ShownScan {
    bool any;
    bool mixed;
    int64_t first_full;
};

static struct ShownScan rows_shown(const struct SimState* sm, const struct Frame* f, const struct FrameOut* o) {
    struct ShownScan r = { false, false, 0 };
    int64_t lo = 0;
    int64_t hi = 0;

    r.any = rows_range(f, o, &lo, &hi);
    if(r.any) {
        lo += sm->tol;
        hi -= sm->tol;
        if(hi < lo) {
            hi = lo;
        }
        r.mixed = co5300_te_fdiv(lo, sm->st) != co5300_te_fdiv(hi, sm->st);
        r.first_full = co5300_te_fdiv(hi, sm->st) + 1;
    }
    return r;
}

/* Leaves the bound for the next frame, as the driver does after it. */
static void sim_finish(struct SimState* sm, const struct Frame* f, struct FrameOut* o) {
    uint16_t y0[MAXR];
    uint16_t y1[MAXR];
    int64_t s[MAXR];
    int64_t e[MAXR];
    uint8_t n = 0;

    for(uint8_t i = 0; i < f->n; i++) {
        if(o->done[i]) {
            y0[n] = f->y0[i];
            y1[n] = f->y1[i];
            s[n] = o->s[i];
            e[n] = o->e[i];
            n++;
        }
    }

    /* Nothing written (every rect failed or was dropped): the previous bound stays, only a new epoch voids it */
    if(n == 0 && sm->c.policy != POL_NONE) {
        if(sm->bound.epoch != o->epoch1) {
            sm->bound.valid = false;
        }
        return;
    }

    bool timed = o->epoch0 == o->epoch1;

    if(o->lost) {
        timed = false;
    }
    bool stamped = timed;

    if(sm->c.policy != POL_STAMPS) {
        stamped = false;
    }

    if(sm->c.policy == POL_NONE) {
        sm->bound.valid = false;
    } else if(sm->c.policy == POL_PLANNED && timed && o->have_plan) {
        sm->bound.at_us = o->ref + (int64_t)o->planned * S_US;
        sm->bound.epoch = o->epoch1;
        sm->bound.valid = true;
        sm->bound.scan_start = true;
    } else {
        sm->bound = co5300_te_frame_finish(n, y0, y1, s, e, o->ref, S_US, TVDL_US, H, MARGIN, stamped, o->epoch1);
    }
}

/* Where the property and runtime tests start: all faults off. */
static struct SimConfig quiet_cfg(void) {
    struct SimConfig c;

    c = (struct SimConfig){ 0 };
    c.lose_from = -1;
    c.blank_step = -1;
    c.off_step = -1;
    c.policy = POL_STAMPS;
    return c;
}

/* ---- Drag frame, target scan range ---- */

ZTEST(te_frame, test_drag_frame_every_phase) {
    for(int set = 0; set < 2; set++) {
        struct Frame f;
        struct PlanBuf pb;

        drag_frame(&f);
        if(set == 0) {
            set_bounds(&f, 100, 300);
        } else {
            set_bounds_drag(&f);
        }

        /* chart, needle, bar, number, slider */
        const uint8_t want[5] = { DRAG_CHART, DRAG_NEEDLE, DRAG_BAR, DRAG_NUMBER, DRAG_SLIDER };
        unsigned phases = 0;
        unsigned searched = 0;

        for(uint32_t age = 0; age < S_US; age += 50) {
            enum co5300_te_frame_result r = plan_frame(&pb, &f, age, 0, MARGIN, GAP, true);

            zassert_equal(r, CO5300_TE_FRAME_OK, "set %d age %u: %d", set, age, r);
            zassert_true(pb.scan >= 0 && pb.scan <= 2, "set %d age %u: scan %d", set, age, pb.scan);
            if(memcmp(pb.order, want, 5) == 0) {
                zassert_mem_equal(
                    pb.order, want, 5, "set %d age %u: order %u %u %u %u %u", set, age, pb.order[0], pb.order[1],
                    pb.order[2], pb.order[3], pb.order[4]
                );
            } else {
                /*
                 * The sorted order misses scan N from the late phases (after about 8.7 ms), so the plan
                 * takes the earlier scan with another order, which D-5 asks for: the smallest N wins.
                 */
                struct BruteResult sorted = brute_force(&f, age, 0, MARGIN, GAP, false);

                zassert_true(!sorted.found || sorted.scan > pb.scan, "set %d age %u: searched for nothing", set, age);
                searched++;
            }

            /* the plan holds on the simulated panel, too */
            struct SimConfig c = quiet_cfg();
            struct SimState sm;
            struct FrameOut o;

            c.wmode = (int)(age / 50U) % 3;
            sim_init(&sm, &c, 7);
            sm.t = (int64_t)age;
            sim_frame(&sm, &f, &o);

            struct ShownScan sh = rows_shown(&sm, &f, &o);

            zassert_equal(o.nw, 5);
            zassert_equal(o.replans, 0);
            zassert_false(sh.mixed, "set %d age %u: mixed", set, age);
            zassert_equal(
                sh.first_full, pb.scan, "set %d age %u: shown by %d, planned %d", set, age, (int)sh.first_full, pb.scan
            );
            phases++;
        }
        zassert_equal(phases, 336U);
        zassert_true(searched < 100, "set %d: the search ran for %u phases", set, searched);
    }
}

ZTEST(te_frame, test_target_scan_range) {
    struct Frame f;
    struct PlanBuf pb;

    /*
     * The class of section 5.4: a chart-like rect and a full-screen one, age above S + b. Scans 0, 1 and
     * 2 all miss a window (the full-screen rect starts too late for scan 2), scan 3 is the first that
     * holds both.
     */
    f = (struct Frame){ 0 };
    add_rect(&f, 0, 139, 466);
    add_rect(&f, 0, 465, 466);
    set_bounds(&f, 100, 300);

    uint32_t age = S_US + S_US * 8U / 100U;
    struct BruteResult want = brute_force(&f, age, 0, MARGIN, GAP, true);

    zassert_true(want.found);
    zassert_equal(want.scan, 3, "scan %d", want.scan);
    zassert_equal(plan_frame(&pb, &f, age, 0, MARGIN, GAP, true), CO5300_TE_FRAME_OK);
    zassert_equal(pb.scan, 3);
    zassert_equal(pb.order[0], 0);

    /* and shifted by the bound: N_min + 3 */
    zassert_equal(plan_frame(&pb, &f, age, 2, MARGIN, GAP, true), CO5300_TE_FRAME_OK);
    zassert_equal(pb.scan, 3);
    zassert_equal(plan_frame(&pb, &f, age, 4, MARGIN, GAP, true), CO5300_TE_FRAME_OK);
    zassert_equal(pb.scan, 4);

    /* Random frames with an old edge: the plan's scan is the first feasible one, never beyond N_last */
    seed_rng(0x7a96e001);

    unsigned plus3 = 0;
    unsigned ok = 0;

    for(unsigned c = 0; c < 6000; c++) {
        random_frame(&f, 2, 4);
        set_bounds(&f, 100, 300);
        for(unsigned i = 0; i < f.n; i++) {
            if(rnd_u32() % 2 == 0) {
                f.x[i] = H; /* long writes make the late scans matter */
            }
        }
        set_bounds(&f, 100, 300);
        age = (uint32_t)rnd_in(S_US, S_US + S_US / 10U);

        int32_t min_scan = (int32_t)(rnd_u32() % 3);
        enum co5300_te_frame_result r = plan_frame(&pb, &f, age, min_scan, MARGIN, GAP, true);

        want = brute_force(&f, age, min_scan, MARGIN, GAP, true);
        if(pb.nodes >= CO5300_TE_FRAME_SEARCH_NODES) {
            continue;
        }
        if(r == CO5300_TE_FRAME_OK) {
            zassert_true(want.found, "case %u: plan found a scan, brute force none", c);
            zassert_equal(pb.scan, want.scan, "case %u: scan %d, first feasible %d", c, pb.scan, want.scan);
            ok++;
            if(pb.scan == min_scan + 3) {
                plus3++;
            }
        } else {
            zassert_equal(r, CO5300_TE_FRAME_NO_WINDOW, "case %u: %d", c, r);
            zassert_false(want.found, "case %u: no window reported, brute force scan %d", c, want.scan);
        }
    }
    zassert_true(ok > 1000, "ok %u", ok);
    zassert_true(plus3 > 0, "no frame needed N_min + 3 in the sweep");
}

/* ---- The property: no start the plan makes tears ---- */

ZTEST(te_frame, test_no_mixed_scan_property) {
    static const uint32_t BELOW[3] = { 0, 50, 150 };
    static const uint32_t ABOVE[3] = { 0, 100, 300 };
    static const int WMODES[3] = { 0, 1, 2 };

    seed_rng(0xc0ffee11);

    unsigned frames = 0;
    unsigned planned = 0;
    unsigned nowin = 0;
    unsigned multi = 0;

    for(unsigned c = 0; c < 4500; c++) {
        struct Frame f;
        struct PlanBuf pb;
        struct SimConfig cf = quiet_cfg();
        uint32_t margin = (c % 3 == 0) ? 0U : (uint32_t)MARGIN;
        uint32_t gap = (uint32_t)rnd_in(0, GAP);

        random_frame(&f, 1, 6);
        set_bounds(&f, BELOW[rnd_u32() % 3], ABOVE[rnd_u32() % 3]);

        uint32_t age = (uint32_t)rnd_in(0, S_US + S_US / 10U);
        int32_t min_scan = (int32_t)(rnd_u32() % 2);
        enum co5300_te_frame_result r = plan_frame(&pb, &f, age, min_scan, margin, GAP, true);

        frames++;
        if(r != CO5300_TE_FRAME_OK) {
            nowin++;
            continue;
        }
        planned++;
        multi += f.n > 1 ? 1U : 0U;

        /*
         * Drive the steps. With the margin the faults of section 5.6 are on: pauses after each 32 KB,
         * a delayed first pixel, an edge stamped late. Without a margin there is nothing to spend.
         */
        if(margin > 0) {
            cf.pause_max = 250;
            cf.delay_max = 100;
            cf.lat_max = 50;
            cf.drift = 2;
        }
        cf.gap_max = gap;
        cf.wmode = WMODES[rnd_u32() % 3];

        struct SimState sm;
        struct FrameOut o;

        sim_init(&sm, &cf, c + 1U);
        sm.tol = margin == 0 ? 1 : 0; /* integer row times are floors: a margin of 0 is exact to a microsecond */
        sm.t = (int64_t)age + hash_rand(sm.salt + 1, 0, cf.lat_max);
        if(age > S_US) {
            sm.late_edge1 = sm.t + 1; /* the next edge is late: the old edge is still the newest */
        }
        sm.bound.valid = false;

        /* the same plan, driven through the steps with min_scan as given */
        struct co5300_te_frame_run run = { .ref_us = edge_stamp(&sm, 0), .scan = pb.scan, .epoch = 1 };
        uint8_t order[MAXR];

        copy_bytes(order, pb.order, f.n);

        struct FrameOut o2;

        o2 = (struct FrameOut){ 0 };
        sm.once_pending = false;
        while(run.k < f.n) {
            struct co5300_te_frame_snap sn = take_snapshot(&sm);
            uint32_t wait = 0;
            enum co5300_te_frame_action act = co5300_te_frame_step(&run, pb.a, pb.b, order, &sn, &wait);
            uint8_t i = order[run.k];

            zassert_true(
                act == CO5300_TE_FRAME_START || act == CO5300_TE_FRAME_WAIT || act == CO5300_TE_FRAME_WAIT_EDGE,
                "case %u: act %d at rect %u of %u (age %u, scan %d)", c, act, run.k, f.n, age, pb.scan
            );
            if(act == CO5300_TE_FRAME_START) {
                zassert_true(write_rect(&sm, &f, i, &o2));
                run.k++;
                run.rounds = 0;
            } else if(act == CO5300_TE_FRAME_WAIT) {
                sm.t += wait;
                run.rounds++;
            } else {
                wait_next_edge(&sm);
                run.rounds++;
            }
        }
        o = o2;
        o.ref = edge_stamp(&sm, 0);

        struct ShownScan sh = rows_shown(&sm, &f, &o);

        zassert_false(
            sh.mixed, "case %u: mixed (n %u, age %u, scan %d, margin %u, wmode %d)", c, f.n, age, pb.scan, margin,
            cf.wmode
        );
        zassert_equal(sh.first_full, pb.scan, "case %u: shown by %d, planned %d", c, (int)sh.first_full, pb.scan);

        /* the closed-form audit agrees, and its scan is the planned one */
        struct co5300_te_frame_verdict v = co5300_te_frame_audit(
            f.n, f.y0, f.y1, o.s, o.e, f.wmax, o.ref, S_US, TVDL_US, H, margin
        );

        zassert_false(v.mixed, "case %u: audit mixed", c);
        zassert_false(v.late, "case %u: audit late", c);
        zassert_equal(
            v.shown_us, o.ref + (int64_t)pb.scan * S_US, "case %u: audit shows scan at %d, planned %d", c,
            (int)((v.shown_us - o.ref) / S_US), pb.scan
        );
    }
    zassert_true(planned > 2500, "planned %u of %u (no window %u)", planned, frames, nowin);
    zassert_true(multi > 1500);
}

/* ---- The plan is the earliest ---- */

ZTEST(te_frame, test_plan_is_earliest) {
    seed_rng(0xea71e5);

    unsigned compared = 0;
    unsigned searched = 0;

    /* every order, n <= 4 every 7 us, n 5-6 every 97 us */
    for(unsigned c = 0; c < 130; c++) {
        struct Frame f;
        struct PlanBuf pb;
        unsigned max_n = c < 100 ? 4U : 6U;
        uint32_t step = c < 100 ? 7U : 97U;

        random_frame(&f, 2, max_n);
        set_bounds(&f, 100, 300);

        for(uint32_t age = 0; age <= S_US + S_US / 10U; age += step) {
            int32_t min_scan = (int32_t)((age / step) % 2U);
            enum co5300_te_frame_result r = plan_frame(&pb, &f, age, min_scan, MARGIN, GAP, true);
            struct BruteResult want = brute_force(&f, age, min_scan, MARGIN, GAP, true);

            if(pb.nodes >= CO5300_TE_FRAME_SEARCH_NODES) {
                continue;
            }
            compared++;
            if(!want.found) {
                zassert_equal(r, CO5300_TE_FRAME_NO_WINDOW, "frame %u age %u: plan %d, no schedule exists", c, age, r);
                continue;
            }
            zassert_equal(
                r, CO5300_TE_FRAME_OK, "frame %u age %u: no window, but scan %d is feasible", c, age, want.scan
            );
            zassert_equal(pb.scan, want.scan, "frame %u age %u: scan %d, earliest %d", c, age, pb.scan, want.scan);
            zassert_true(is_permutation(pb.order, f.n));

            int64_t first = 0;
            int64_t end = 0;

            zassert_true(ref_follow(&f, pb.order, age, pb.scan, MARGIN, GAP, &first, &end), "frame %u age %u", c, age);
            if(pb.nodes > 0) {
                /* the search ran: its order ends earliest */
                zassert_equal(end, want.end, "frame %u age %u: end %d, best %d", c, age, (int)end, (int)want.end);
                searched++;
            }
        }
    }
    zassert_true(compared > 5000);
    zassert_true(searched > 0, "the search never ran");

    /* n > 6: only the sorted order is tried */
    for(unsigned c = 0; c < 40; c++) {
        struct Frame f;
        struct PlanBuf pb;

        random_frame(&f, 7, 11);
        for(unsigned i = 0; i < f.n; i++) {
            /* small areas, so that a frame is feasible now and then */
            f.y1[i] = (uint16_t)(f.y0[i] + (f.y1[i] - f.y0[i]) / 8U);
            f.x[i] = (uint16_t)(f.x[i] / 8U + 1U);
        }
        set_bounds(&f, 100, 300);
        for(uint32_t age = 0; age <= S_US + S_US / 10U; age += 31) {
            enum co5300_te_frame_result r = plan_frame(&pb, &f, age, 0, MARGIN, GAP, true);
            struct BruteResult want = brute_force(&f, age, 0, MARGIN, GAP, false);

            zassert_equal(pb.nodes, 0U, "no search ABOVE %d rects", CO5300_TE_FRAME_SEARCH_MAX);
            zassert_equal(r == CO5300_TE_FRAME_OK, want.found, "frame %u age %u", c, age);
            if(want.found) {
                zassert_equal(
                    pb.scan, want.scan, "frame %u age %u: scan %d, sorted order's %d", c, age, pb.scan, want.scan
                );
                compared++;
            }
        }
    }
}

/* ---- The search budget ---- */

ZTEST(te_frame, test_search_budget) {
    seed_rng(0xb0d6e7);

    unsigned heavy = 0;
    unsigned nowin_frames = 0;

    for(unsigned c = 0; c < 4000; c++) {
        struct Frame f;
        struct PlanBuf pb;

        /* small areas that fit anywhere, and two full-height bars that share no window: many prefixes hold
         * before the frame fails, which is what spends the budget */
        f = (struct Frame){ 0 };
        add_rect(&f, 0, H - 1, (uint16_t)rnd_in(2, 40));
        add_rect(&f, 0, H - 1, (uint16_t)rnd_in(2, 40));
        for(unsigned i = (unsigned)rnd_in(2, 4); i > 0; i--) {
            uint16_t h = (uint16_t)rnd_in(1, 40);
            uint16_t y0 = (uint16_t)rnd_in(0, H - h);

            add_rect(&f, y0, (uint16_t)(y0 + h - 1), (uint16_t)rnd_in(1, 100));
        }
        for(uint8_t i = f.n - 1U; i > 0; i--) {
            /* the bars in any position */
            uint8_t j = (uint8_t)rnd_in(0, i);
            uint16_t t = 0;

            t = f.y0[i];
            f.y0[i] = f.y0[j];
            f.y0[j] = t;
            t = f.y1[i];
            f.y1[i] = f.y1[j];
            f.y1[j] = t;
            t = f.x[i];
            f.x[i] = f.x[j];
            f.x[j] = t;
        }
        set_bounds(&f, 100, 300);

        enum co5300_te_frame_result r0 = plan_frame(&pb, &f, 0, 0, MARGIN, GAP, true);
        uint32_t nodes0 = pb.nodes;

        zassert_true(nodes0 <= CO5300_TE_FRAME_SEARCH_NODES, "case %u: %u nodes", c, nodes0);
        if(r0 == CO5300_TE_FRAME_NO_WINDOW) {
            nowin_frames++;
            if(nodes0 > 1000) {
                heavy++;
            }
            /* no window from any phase: the verdict and its cost do not depend on the age */
            for(uint32_t age = 61; age <= S_US + S_US / 10U; age += 1013) {
                enum co5300_te_frame_result r = plan_frame(&pb, &f, age, 0, MARGIN, GAP, true);

                zassert_equal(r, CO5300_TE_FRAME_NO_WINDOW, "case %u age %u: %d", c, age, r);
                zassert_equal(pb.nodes, nodes0, "case %u age %u: %u nodes, %u at age 0", c, age, pb.nodes, nodes0);
            }
        } else if(r0 == CO5300_TE_FRAME_OK && nodes0 < CO5300_TE_FRAME_SEARCH_NODES) {
            for(uint32_t age = 61; age <= S_US + S_US / 10U; age += 1013) {
                zassert_equal(
                    plan_frame(&pb, &f, age, 0, MARGIN, GAP, true), CO5300_TE_FRAME_OK, "case %u age %u", c, age
                );
            }
        }
    }
    zassert_true(nowin_frames > 100, "frames without a window: %u", nowin_frames);
    zassert_true(heavy > 0, "no frame spent more than half the budget");

    /* six full-height areas of 6 ms: every prefix of two or three holds, no sixth does */
    struct Frame f;
    struct PlanBuf pb;

    f = (struct Frame){ 0 };
    for(unsigned i = 0; i < 6; i++) {
        add_rect(&f, 0, 465, 150);
    }
    set_bounds(&f, 100, 300);
    zassert_equal(plan_frame(&pb, &f, 0, 0, MARGIN, GAP, true), CO5300_TE_FRAME_NO_WINDOW);
    zassert_true(pb.nodes <= CO5300_TE_FRAME_SEARCH_NODES);
}

/* ---- Frames across frames ---- */

struct StreamStats {
    unsigned frames;
    unsigned decreases;
    unsigned split;
    unsigned nowin;
    unsigned fallback;
    unsigned lost;
    unsigned mixed_clean;
};

static void random_stream_frame(struct Frame* f) {
    *f = (struct Frame){ 0 };
    switch(rnd_u32() % 10) {
        case 0:
            /* the bar's wrap: a sliver and the bar, both full height */
            add_rect(f, 0, H - 1, (uint16_t)rnd_in(2, 11));
            add_rect(f, 0, H - 1, 40);
            break;
        case 1:
            add_rect(f, 0, H - 1, H);
            break;
        case 2:
            drag_frame(f);
            break;
        default:
            random_frame(f, 1, 5);
            break;
    }
    set_bounds(f, 100, 300);
}

static struct StreamStats run_stream(enum BoundPolicy policy, uint32_t seed, unsigned frames) {
    struct SimConfig c = quiet_cfg();
    struct SimState sm;
    struct StreamStats st = { 0, 0, 0, 0, 0, 0, 0 };
    int64_t prev_full = INT64_MIN;
    uint32_t epoch = 1;

    c.policy = policy;
    c.lat_max = 50;
    c.late_max = 1000;
    c.delay_max = 100;
    c.pause_max = 250;
    c.drift = 2;
    c.jitter = 20;
    c.gap_max = GAP;
    c.stall_between = 7000;
    c.stall_between_pct = 6;
    c.stall_inside = 3000;
    c.stall_inside_pct = 3;
    c.lose_pct = 2;
    c.invalid_start_pct = 4;
    c.wmode = 2;
    c.eio_pct = 2;
    c.early_wake_pct = 3;
    sim_init(&sm, &c, seed);
    seed_rng(seed);
    sm.t = 20000;

    for(unsigned n = 0; n < frames; n++) {
        struct Frame f;
        struct FrameOut o;

        random_stream_frame(&f);
        sm.t += rnd_in(0, 5000); /* the next frame is ready 0-5 ms after the last one ended */
        sim_frame(&sm, &f, &o);
        sim_finish(&sm, &f, &o);

        struct ShownScan sh = rows_shown(&sm, &f, &o);

        st.frames++;
        st.nowin += o.nowin ? 1U : 0U;
        st.fallback += o.fallback ? 1U : 0U;
        st.lost += o.lost ? 1U : 0U;
        st.split += o.misses > 0 ? 1U : 0U;
        if(o.epoch1 != epoch) {
            epoch = o.epoch1;
            prev_full = INT64_MIN;
        }
        if(sh.any) {
            if(sh.first_full < prev_full) {
                st.decreases++;
            }
            prev_full = sh.first_full;
        }
    }
    return st;
}

ZTEST(te_frame, test_cross_frame_stream) {
    struct StreamStats real = run_stream(POL_STAMPS, 0x57a3a001, 6000);

    zassert_equal(real.frames, 6000U);
    zassert_equal(
        real.decreases, 0U, "the bound from the stamps: the first full scan decreased %u times", real.decreases
    );
    /* the stream really contains what the test claims */
    zassert_true(real.nowin > 100, "no-window frames: %u", real.nowin);
    zassert_true(real.fallback > 100, "edge-rule frames: %u", real.fallback);
    zassert_true(real.split > 20, "frames with a missed window: %u", real.split);
    zassert_true(real.lost > 100, "frames that lost their timing: %u", real.lost);

    /* with min_scan forced to 0 the same stream decreases: the test can fail */
    struct StreamStats none = run_stream(POL_NONE, 0x57a3a001, 6000);

    zassert_true(none.decreases > 0, "no decreases without a bound");

    /* the counterexample of section 5.8, with the planned scan as the bound and with the stamps */
    for(int pol = 0; pol < 3; pol++) {
        static const enum BoundPolicy POLICIES[3] = { POL_STAMPS, POL_PLANNED, POL_NONE };
        struct SimConfig c = quiet_cfg();
        struct SimState sm;
        struct Frame a;
        struct Frame b;
        struct FrameOut oa;
        struct FrameOut ob;

        c.policy = POLICIES[pol];
        c.wmode = 1;
        c.gap_max = 0;
        c.stall_once = 6000; /* a stall after the first rect */
        sim_init(&sm, &c, 5);
        sm.t = 100;

        a = (struct Frame){ 0 };
        add_rect(&a, 0, 199, 466); /* A */
        add_rect(&a, 20, 29, 40);  /* B */
        set_bounds(&a, 100, 300);
        b = (struct Frame){ 0 };
        add_rect(&b, 400, 409, 40); /* C */
        set_bounds(&b, 100, 300);

        sim_frame(&sm, &a, &oa);
        zassert_true(oa.misses > 0, "policy %d: B should have missed its window", pol);
        zassert_equal(oa.nw, 2);
        sim_finish(&sm, &a, &oa);

        struct ShownScan sa = rows_shown(&sm, &a, &oa);

        sm.t += 100;
        sim_frame(&sm, &b, &ob);
        sim_finish(&sm, &b, &ob);

        struct ShownScan sb = rows_shown(&sm, &b, &ob);

        if(pol == 0) {
            zassert_true(
                sb.first_full >= sa.first_full, "stamps: C %d before the frame %d", (int)sb.first_full,
                (int)sa.first_full
            );
        } else {
            zassert_true(
                sb.first_full < sa.first_full, "policy %d: C %d, frame %d: expected a decrease", pol,
                (int)sb.first_full, (int)sa.first_full
            );
        }
    }
}

/* ---- The runtime sim with every fault ---- */

ZTEST(te_frame, test_frame_runtime_sim) {
    struct {
        const char* name;
        struct SimConfig c;
        unsigned frames;
    } runs[6];
    unsigned clean = 0;
    unsigned unclean = 0;

    for(unsigned i = 0; i < 6; i++) {
        runs[i].c = quiet_cfg();
        runs[i].c.wmode = 2;
        runs[i].c.lat_max = 50;
        runs[i].c.delay_max = 100;
        runs[i].c.pause_max = 250;
        runs[i].c.drift = 2;
        runs[i].c.gap_max = GAP;
        runs[i].c.eio_pct = 1;
        runs[i].frames = 600;
    }
    runs[0].name = "late edges and jitter";
    runs[0].c.late_max = 1000;
    runs[0].c.jitter = 20;
    runs[1].name = "lost edges";
    runs[1].c.lose_pct = 10;
    runs[2].name = "stalls between and inside";
    runs[2].c.stall_between = 8000;
    runs[2].c.stall_between_pct = 15;
    runs[2].c.stall_inside = 3000;
    runs[2].c.stall_inside_pct = 6;
    runs[3].name = "short sleeps, rounds used up";
    runs[3].c.early_wake_pct = 60;
    runs[3].c.eio_pct = 10;
    runs[4].name = "blanking and the window rule off";
    runs[4].c.blank_step = 40;
    runs[4].c.off_step = 400;
    runs[5].name = "stale edges";
    runs[5].c.lose_from = 3;

    for(unsigned r = 0; r < 6; r++) {
        struct SimState sm;
        int64_t prev_full = INT64_MIN;
        uint32_t epoch = 1;

        sim_init(&sm, &runs[r].c, 100U + r);
        seed_rng(0xf00d0000U + r);
        sm.t = 20000;
        for(unsigned n = 0; n < runs[r].frames; n++) {
            struct Frame f;
            struct FrameOut o;

            /* a run with a fault that fires once or keeps firing is re-armed per 40 frames */
            if(n % 40 == 0 && r >= 4) {
                struct SimConfig c = runs[r].c;

                sim_init(&sm, &c, 1000U * r + n);
                sm.t = 20000 + (int64_t)n * 100000;
                sm.t -= sm.t % sm.st; /* edges and the clock stay in step */
                prev_full = INT64_MIN;
                epoch = 1;
                if(r == 5) {
                    /* the edges stop for good a few scans in */
                    sm.c.lose_from = sm.t / sm.st + 1;
                }
                if(r == 4 && (n / 40) % 2 == 1) {
                    sm.c.blank_step = 6;
                    sm.c.off_step = 14;
                }
            }
            random_stream_frame(&f);
            sm.t += rnd_in(0, 5000);
            sim_frame(&sm, &f, &o);
            sim_finish(&sm, &f, &o);

            /* every rect is written once or reported failed */
            for(uint8_t i = 0; i < f.n; i++) {
                zassert_true(
                    o.done[i] != o.failed[i], "%s, frame %u rect %u: done %d failed %d", runs[r].name, n, i, o.done[i],
                    o.failed[i]
                );
            }
            zassert_equal(o.nw + o.nfailed, f.n, "%s, frame %u", runs[r].name, n);

            struct ShownScan sh = rows_shown(&sm, &f, &o);
            struct co5300_te_frame_verdict v = co5300_te_frame_audit(
                f.n, f.y0, f.y1, o.s, o.e, f.wmax, o.ref, S_US, TVDL_US, H, MARGIN
            );
            bool no_event = true;

            if(o.replans != 0 || o.misses != 0 || o.epoch0 != o.epoch1 || o.nfailed != 0) {
                no_event = false;
            }
            if(o.lost || o.fallback || o.nowin) {
                no_event = false;
            }

            if(no_event && !v.late && sh.any) {
                zassert_false(
                    sh.mixed, "%s, frame %u: mixed without a reported miss (n %u, scan %d)", runs[r].name, n, f.n,
                    o.planned
                );
                clean++;
            } else {
                unclean++;
            }
            if(o.epoch1 != epoch) {
                epoch = o.epoch1;
                prev_full = INT64_MIN;
            }
            if(sh.any) {
                zassert_true(
                    sh.first_full >= prev_full, "%s, frame %u: the first full scan went from %d to %d", runs[r].name, n,
                    (int)prev_full, (int)sh.first_full
                );
                prev_full = sh.first_full;
            }
        }
    }
    zassert_true(clean > 1000, "clean frames: %u", clean);
    zassert_true(unclean > 200, "frames with an event: %u", unclean);
}

/* ---- Frames without a window ---- */

ZTEST(te_frame, test_no_window_cases) {
    struct Frame f;
    struct PlanBuf pb;

    /* the bar's wrap frame: a 2-11 px sliver and the 40 px bar, both full height */
    for(uint16_t w = 2; w <= 11; w++) {
        f = (struct Frame){ 0 };
        add_rect(&f, 0, H - 1, w);
        add_rect(&f, 0, H - 1, 40);
        set_bounds(&f, 100, 300);
        for(uint32_t age = 0; age <= S_US + S_US / 10U; age += 211) {
            zassert_equal(
                plan_frame(&pb, &f, age, 0, MARGIN, GAP, true), CO5300_TE_FRAME_NO_WINDOW, "wrap w %u age %u", w, age
            );
        }
    }

    /* each of them has a window alone */
    f = (struct Frame){ 0 };
    add_rect(&f, 0, H - 1, 40);
    set_bounds(&f, 100, 300);
    zassert_equal(plan_frame(&pb, &f, 0, 0, MARGIN, GAP, true), CO5300_TE_FRAME_OK);

    /* two full-height 40 px bars */
    f = (struct Frame){ 0 };
    add_rect(&f, 0, H - 1, 40);
    add_rect(&f, 0, H - 1, 40);
    set_bounds(&f, 100, 300);
    for(uint32_t age = 0; age <= S_US + S_US / 10U; age += 211) {
        zassert_equal(plan_frame(&pb, &f, age, 0, MARGIN, GAP, true), CO5300_TE_FRAME_NO_WINDOW, "age %u", age);
        zassert_equal(plan_frame(&pb, &f, age, 1, MARGIN, GAP, true), CO5300_TE_FRAME_NO_WINDOW, "age %u", age);
    }

    /* every full-width strip at 2 B/px, with the bounds of a class never seen */
    struct co5300_te_wbounds wb;
    unsigned strips = 0;

    co5300_te_wbounds_init(&wb);
    for(unsigned h = 1; h <= H; h += 3) {
        for(uint16_t y0 = 0; y0 + h <= H; y0 += 7) {
            f = (struct Frame){ 0 };
            add_rect(&f, y0, (uint16_t)(y0 + h - 1), H);
            co5300_te_wbounds_get(&wb, rect_bytes(&f, 0), NS_PER_BYTE, &f.wmin[0], &f.wmax[0]);
            for(uint32_t age = 0; age <= S_US; age += 4200) {
                zassert_equal(
                    plan_frame(&pb, &f, age, 0, MARGIN, GAP, true), CO5300_TE_FRAME_OK, "strip %u-%u age %u", y0,
                    y0 + h - 1U, age
                );
            }
            strips++;
        }
    }
    zassert_true(strips > 5000);

    /* a 1-3 px sliver alone: no window for the plan, the plain edge rule through the decide */
    for(uint16_t w = 1; w <= 3; w++) {
        f = (struct Frame){ 0 };
        add_rect(&f, 0, H - 1, w);
        co5300_te_wbounds_get(&wb, rect_bytes(&f, 0), NS_PER_BYTE, &f.wmin[0], &f.wmax[0]);
        zassert_equal(plan_frame(&pb, &f, 100, 0, MARGIN, GAP, true), CO5300_TE_FRAME_NO_WINDOW, "sliver %u", w);

        struct co5300_te_window_in in = {
            .age_us = 100,
            .s_us = S_US,
            .tvdl_us = TVDL_US,
            .panel_h = H,
            .y0 = 0,
            .y1 = H - 1,
            .w_min_us = f.wmin[0],
            .w_max_us = f.wmax[0],
            .margin_us = MARGIN,
            .valid = true,
        };

        zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_FALLBACK, "sliver %u", w);
    }
}

/* ---- The audit ---- */

/* A per-scan loop over the first and the last row of every rect, scan by scan. */
static void
scan_loop(const struct Frame* f, const struct FrameOut* o, int64_t ref, int64_t s, bool* mixed, int64_t* first_full) {
    bool have = false;

    *mixed = false;
    *first_full = 0;
    for(int64_t k = -2; k < 12; k++) {
        bool any_new = false;
        bool any_old = false;

        for(uint8_t i = 0; i < f->n; i++) {
            // ties count as old: the scan was first
            bool new0 = o->s[i] < ref + k * s + row_t(f->y0[i]);
            bool new1 = o->e[i] < ref + k * s + row_t(f->y1[i]);

            if(new0 || new1) {
                any_new = true;
            }
            if(!new0 || !new1) {
                any_old = true;
            }
        }
        if(any_new && any_old) {
            *mixed = true;
        }
        if(!any_old && !have) {
            *first_full = k;
            have = true;
        }
    }
}

ZTEST(te_frame, test_audit_closed_form) {
    seed_rng(0xa0d17001);

    unsigned mixed_seen = 0;
    unsigned clean_seen = 0;
    unsigned late_seen = 0;

    for(unsigned c = 0; c < 1500; c++) {
        struct Frame f;
        struct FrameOut o;
        struct SimState sm;
        struct SimConfig cf = quiet_cfg();
        int64_t t = rnd_in(0, 2LL * S_US);

        random_frame(&f, 1, 4);
        set_bounds(&f, 100, 300);
        sim_init(&sm, &cf, c);
        o = (struct FrameOut){ 0 };

        /* straight-line writes: a stall between rects, a stall inside is a slower write */
        for(uint8_t i = 0; i < f.n; i++) {
            int64_t w = rnd_in(f.wmin[i], f.wmax[i]);

            if(rnd_u32() % 5 == 0) {
                w += rnd_in(0, 2500);
            }
            o.s[i] = t;
            o.e[i] = t + w;
            o.done[i] = true;
            t = o.e[i] + (rnd_u32() % 3 == 0 ? rnd_in(0, 6000) : rnd_in(0, 60));
        }
        o.ref = 0;

        struct ShownScan sh = rows_shown(&sm, &f, &o);
        struct co5300_te_frame_verdict v = co5300_te_frame_audit(
            f.n, f.y0, f.y1, o.s, o.e, f.wmax, 0, S_US, TVDL_US, H, MARGIN
        );
        bool loop_mixed = false;
        int64_t loop_full = 0;

        scan_loop(&f, &o, 0, S_US, &loop_mixed, &loop_full);
        zassert_equal(v.mixed, sh.mixed, "case %u: audit %d, rows %d", c, v.mixed, sh.mixed);
        zassert_equal(v.mixed, loop_mixed, "case %u: audit %d, scan loop %d", c, v.mixed, loop_mixed);
        zassert_equal(
            v.shown_us, sh.first_full * S_US, "case %u: audit scan %d, rows %d", c, (int)(v.shown_us / S_US),
            (int)sh.first_full
        );
        zassert_equal(
            v.shown_us, loop_full * S_US, "case %u: audit scan %d, loop %d", c, (int)(v.shown_us / S_US), (int)loop_full
        );
        mixed_seen += v.mixed ? 1U : 0U;
        clean_seen += v.mixed ? 0U : 1U;
        late_seen += v.late ? 1U : 0U;
    }
    zassert_true(mixed_seen > 100 && clean_seen > 100, "mixed %u clean %u", mixed_seen, clean_seen);
    zassert_true(late_seen > 100, "late %u", late_seen);

    /* the photo case: the chart in one scan, the needle's box in the next */
    struct Frame f;
    struct PlanBuf pb;

    f = (struct Frame){ 0 };
    add_rect(&f, 32, 185, 466); /* chart */
    add_rect(&f, 19, 238, 120); /* needle's box */
    set_bounds(&f, 100, 300);

    int64_t a[2];
    int64_t b[2];
    int64_t s[2];
    int64_t e[2];

    for(uint8_t i = 0; i < 2; i++) {
        ref_bounds(&f, i, MARGIN, &a[i], &b[i]);
    }
    /* each on its own window: the chart for scan 1, the box for scan 2 */
    s[0] = b[0];
    e[0] = s[0] + f.wmax[0];
    s[1] = S_US + b[1];
    e[1] = s[1] + f.wmax[1];

    struct co5300_te_frame_verdict v = co5300_te_frame_audit(2, f.y0, f.y1, s, e, f.wmax, 0, S_US, TVDL_US, H, MARGIN);

    zassert_true(v.mixed, "the photo case is mixed");
    zassert_false(v.late);

    /* planned as one frame: not */
    zassert_equal(plan_frame(&pb, &f, 0, 0, MARGIN, GAP, true), CO5300_TE_FRAME_OK);
    {
        int64_t t = 0;

        for(uint8_t k = 0; k < 2; k++) {
            uint8_t i = pb.order[k];
            int64_t lo = ((int64_t)pb.scan - 1) * S_US + b[i];

            s[i] = t > lo ? t : lo;
            e[i] = s[i] + f.wmax[i];
            t = e[i] + GAP;
        }
    }
    v = co5300_te_frame_audit(2, f.y0, f.y1, s, e, f.wmax, 0, S_US, TVDL_US, H, MARGIN);
    zassert_false(
        v.mixed, "planned as one frame, not mixed: scan %d order %u %u s %d %d e %d %d", pb.scan, pb.order[0],
        pb.order[1], (int)s[0], (int)s[1], (int)e[0], (int)e[1]
    );

    /* ties count as old: both ends written exactly when scan 0 reads them are old for scan 0 */
    uint16_t y0 = 100;
    uint16_t y1 = 200;
    int64_t ts[1] = { row_t(y0) };
    int64_t te[1] = { row_t(y1) };
    uint32_t wm[1] = { 100000 };

    v = co5300_te_frame_audit(1, &y0, &y1, ts, te, wm, 0, S_US, TVDL_US, H, 0);
    zassert_false(v.mixed);
    zassert_equal(v.shown_us, S_US, "both ties are old, so scan 1 is the first that shows all of it");
    /* one microsecond earlier the first row is new for scan 0 while the last is still old */
    ts[0] = row_t(y0) - 1;
    v = co5300_te_frame_audit(1, &y0, &y1, ts, te, wm, 0, S_US, TVDL_US, H, 0);
    zassert_true(v.mixed);

    /* S = 0: all false */
    v = co5300_te_frame_audit(1, &y0, &y1, ts, te, wm, 0, 0, TVDL_US, H, 0);
    zassert_false(v.mixed);
    zassert_false(v.late);
    zassert_equal(v.shown_us, 0);
    v = co5300_te_frame_audit(0, &y0, &y1, ts, te, wm, 0, S_US, TVDL_US, H, 0);
    zassert_false(v.mixed || v.late);
}

ZTEST(te_frame, test_finish_and_min_scan) {
    uint16_t y0[2] = { 32, 19 };
    uint16_t y1[2] = { 185, 238 };
    int64_t s[2] = { 1611, 9900 };
    int64_t e[2] = { 9547, 12960 };
    struct co5300_te_frame_bound b;

    /* with timing: the first scan that showed all of it, with the margin */
    b = co5300_te_frame_finish(2, y0, y1, s, e, 1000, S_US, TVDL_US, H, MARGIN, true, 3);
    zassert_true(b.valid && b.scan_start);
    zassert_equal(b.epoch, 3U);
    zassert_equal(b.at_us, 1000 + S_US);

    /* a stamp within the margin of the next boundary pushes the bound to the next scan */
    s[1] = 1000 + S_US + row_t(19) - MARGIN;
    e[1] = s[1] + 3000;
    b = co5300_te_frame_finish(2, y0, y1, s, e, 1000, S_US, TVDL_US, H, MARGIN, true, 3);
    zassert_equal(b.at_us, 1000 + 2LL * S_US);

    /* without timing: the end of the last rect, rounded up to a scan later */
    b = co5300_te_frame_finish(2, y0, y1, s, e, 1000, S_US, TVDL_US, H, MARGIN, false, 3);
    zassert_true(b.valid);
    zassert_false(b.scan_start);
    zassert_equal(b.at_us, e[1] > e[0] ? e[1] : e[0]);
    zassert_false(co5300_te_frame_finish(0, y0, y1, s, e, 0, S_US, TVDL_US, H, MARGIN, true, 3).valid);

    /* N_min: nearest scan for a scan start, the first scan after it for an end */
    struct co5300_te_frame_bound sb = {
        .at_us = 50000 + 2LL * S_US - 40, .epoch = 3, .valid = true, .scan_start = true
    };
    struct co5300_te_frame_bound eb = { .at_us = 50000 + 2LL * S_US, .epoch = 3, .valid = true, .scan_start = false };

    zassert_equal(co5300_te_frame_min_scan(&sb, 50000, S_US, 3), 2);
    /* a scan start a little after the scan (the new edge's stamp is late): still that scan, not the next */
    sb.at_us = 50000 + 2LL * S_US + 40;
    zassert_equal(co5300_te_frame_min_scan(&sb, 50000, S_US, 3), 2);
    sb.at_us = 50000 + 2LL * S_US - 40;
    zassert_equal(co5300_te_frame_min_scan(&eb, 50000, S_US, 3), 3);
    eb.at_us = 50000 + 2LL * S_US - 1;
    zassert_equal(co5300_te_frame_min_scan(&eb, 50000, S_US, 3), 2);
    /* the bound is in the past, from another epoch, void, or the period is unknown: nothing */
    zassert_equal(co5300_te_frame_min_scan(&sb, 50000 + 10LL * S_US, S_US, 3), 0);
    zassert_equal(co5300_te_frame_min_scan(&sb, 50000, S_US, 4), 0);
    sb.valid = false;
    zassert_equal(co5300_te_frame_min_scan(&sb, 50000, S_US, 3), 0);
    sb.valid = true;
    zassert_equal(co5300_te_frame_min_scan(&sb, 50000, 0, 3), 0);
    zassert_equal(co5300_te_frame_min_scan(NULL, 50000, S_US, 3), 0);
}

/* ---- The step ---- */

ZTEST(te_frame, test_step_actions) {
    int64_t a[1] = { -8245 };
    int64_t b[1] = { 500 };
    uint8_t order[1] = { 0 };
    struct co5300_te_frame_run run = { .ref_us = 1000, .scan = 1, .epoch = 5, .k = 0 };
    struct co5300_te_frame_snap sn = {
        .now_us = 1000, .fall_us = 1000, .s_us = S_US, .tvdl_us = TVDL_US, .epoch = 5, .valid = true
    };
    uint32_t wait = 99;

    /* scan 1: window [500, 16800 - 8245) after the edge */
    zassert_equal(co5300_te_frame_step(&run, a, b, order, &sn, &wait), CO5300_TE_FRAME_WAIT);
    zassert_equal(wait, 500U);
    sn.now_us = 1500;
    zassert_equal(co5300_te_frame_step(&run, a, b, order, &sn, &wait), CO5300_TE_FRAME_START);
    zassert_equal(wait, 0U);
    sn.now_us = 1000 + S_US - 8245 - 1;
    zassert_equal(co5300_te_frame_step(&run, a, b, order, &sn, &wait), CO5300_TE_FRAME_START);
    sn.now_us = 1000 + S_US - 8245;
    zassert_equal(co5300_te_frame_step(&run, a, b, order, &sn, &wait), CO5300_TE_FRAME_REPLAN);

    /* a newer edge: the target is counted from it, by time */
    sn.fall_us = 1000 + S_US + 7;
    sn.now_us = sn.fall_us + 600;
    run.scan = 2;
    zassert_equal(co5300_te_frame_rel_scan(&run, sn.fall_us, S_US), 1);
    zassert_equal(co5300_te_frame_step(&run, a, b, order, &sn, &wait), CO5300_TE_FRAME_START);

    /* a lost edge: the old one is still the newest, 1.05 scans old; the same absolute time */
    sn.fall_us = 1000;
    sn.now_us = 1000 + S_US + 600;
    run.scan = 2;
    zassert_equal(co5300_te_frame_step(&run, a, b, order, &sn, &wait), CO5300_TE_FRAME_START);
    sn.now_us = 1000 + S_US + 200;
    zassert_equal(co5300_te_frame_step(&run, a, b, order, &sn, &wait), CO5300_TE_FRAME_WAIT);
    zassert_equal(wait, 300U);

    /* a wait across the next edge is a wait for the edge */
    run.scan = 3;
    run.ref_us = 1000;
    sn.fall_us = 1000;
    sn.now_us = 1000 + 3000;
    zassert_equal(co5300_te_frame_step(&run, a, b, order, &sn, &wait), CO5300_TE_FRAME_WAIT_EDGE);
    zassert_equal(wait, S_US - 3000U);

    /* wait rounds used up are a miss; after the re-plans it is lost timing */
    run.scan = 1;
    sn.fall_us = 1000;
    sn.now_us = 1100;
    run.rounds = CO5300_TE_FRAME_MAX_WAITS;
    zassert_equal(co5300_te_frame_step(&run, a, b, order, &sn, &wait), CO5300_TE_FRAME_REPLAN);
    run.replans = CO5300_TE_FRAME_MAX_REPLANS;
    zassert_equal(co5300_te_frame_step(&run, a, b, order, &sn, &wait), CO5300_TE_FRAME_LOST);
    run.rounds = 0;
    run.replans = 0;

    /* timing lost: another epoch, not valid, a stale edge, an unknown period */
    sn.epoch = 6;
    zassert_equal(co5300_te_frame_step(&run, a, b, order, &sn, &wait), CO5300_TE_FRAME_LOST);
    sn.epoch = 5;
    sn.valid = false;
    zassert_equal(co5300_te_frame_step(&run, a, b, order, &sn, &wait), CO5300_TE_FRAME_LOST);
    sn.valid = true;
    sn.now_us = 1000 + S_US + S_US / 10 + 1;
    zassert_equal(co5300_te_frame_step(&run, a, b, order, &sn, &wait), CO5300_TE_FRAME_LOST);
    sn.now_us = 1100;
    sn.s_us = 0;
    zassert_equal(co5300_te_frame_step(&run, a, b, order, &sn, &wait), CO5300_TE_FRAME_LOST);
}

/* ---- Order search ---- */

ZTEST(te_frame, test_order_search) {
    seed_rng(0x0de45e01);

    unsigned searched = 0;

    /* n <= 6 against brute force; rects 0 and 1 are identical, so the search must keep their index order */
    for(unsigned c = 0; c < 1500; c++) {
        struct Frame f;
        struct PlanBuf pb;

        random_frame(&f, 3, 6);
        f.y0[1] = f.y0[0];
        f.y1[1] = f.y1[0];
        f.x[1] = f.x[0];
        set_bounds(&f, 100, 300);

        uint32_t age = (uint32_t)rnd_in(0, S_US);
        enum co5300_te_frame_result r = plan_frame(&pb, &f, age, 0, MARGIN, GAP, true);
        struct BruteResult want = brute_force(&f, age, 0, MARGIN, GAP, true);

        if(pb.nodes >= CO5300_TE_FRAME_SEARCH_NODES) {
            continue;
        }
        zassert_equal(r == CO5300_TE_FRAME_OK, want.found, "case %u", c);
        if(r != CO5300_TE_FRAME_OK) {
            continue;
        }
        zassert_true(is_permutation(pb.order, f.n), "case %u", c);
        zassert_equal(pb.scan, want.scan);
        if(pb.nodes > 0) {
            int64_t first = 0;
            int64_t end = 0;

            searched++;
            zassert_true(ref_follow(&f, pb.order, age, pb.scan, MARGIN, GAP, &first, &end));
            zassert_equal(end, want.end, "case %u: search ends at %d, best %d", c, (int)end, (int)want.end);
        }
        {
            uint8_t pos0 = 0;
            uint8_t pos1 = 0;

            for(uint8_t k = 0; k < f.n; k++) {
                pos0 = pb.order[k] == 0 ? k : pos0;
                pos1 = pb.order[k] == 1 ? k : pos1;
            }
            zassert_true(pos0 < pos1, "case %u: identical rects 0 and 1 swapped", c);
        }
    }
    zassert_true(searched > 50, "searched %u", searched);

    /* identical rects keep the identity order */
    struct Frame f;
    struct PlanBuf pb;

    f = (struct Frame){ 0 };
    for(unsigned i = 0; i < 6; i++) {
        add_rect(&f, 100, 120, 30);
    }
    set_bounds(&f, 100, 300);
    zassert_equal(plan_frame(&pb, &f, 0, 0, MARGIN, GAP, true), CO5300_TE_FRAME_OK);
    for(uint8_t i = 0; i < 6; i++) {
        zassert_equal(pb.order[i], i);
    }

    /* 32 small rects: only the sorted order is tried, and it is a valid order */
    f = (struct Frame){ 0 };
    for(unsigned i = 0; i < 32; i++) {
        add_rect(&f, (uint16_t)(i * 14), (uint16_t)(i * 14 + 9), 20);
    }
    set_bounds(&f, 100, 300);
    for(uint32_t age = 0; age < S_US; age += 1000) {
        zassert_equal(plan_frame(&pb, &f, age, 0, MARGIN, GAP, true), CO5300_TE_FRAME_OK, "age %u", age);
        zassert_true(is_permutation(pb.order, 32), "age %u", age);
        zassert_equal(pb.nodes, 0U);
    }

    /* a full budget still ends with a valid order or a verdict, never more nodes than the budget */
    unsigned exhausted = 0;

    for(unsigned c = 0; c < 3000 && exhausted < 5; c++) {
        random_frame(&f, 6, 6);
        for(unsigned i = 0; i < f.n; i++) {
            f.y1[i] = (uint16_t)(f.y0[i] + (f.y1[i] - f.y0[i]) / 3U);
        }
        set_bounds(&f, 100, 300);

        enum co5300_te_frame_result r = plan_frame(&pb, &f, 0, 0, MARGIN, GAP, true);

        zassert_true(pb.nodes <= CO5300_TE_FRAME_SEARCH_NODES);
        if(pb.nodes == CO5300_TE_FRAME_SEARCH_NODES) {
            exhausted++;
            if(r == CO5300_TE_FRAME_OK) {
                int64_t first = 0;
                int64_t end = 0;

                zassert_true(is_permutation(pb.order, f.n));
                zassert_true(ref_follow(&f, pb.order, 0, pb.scan, MARGIN, GAP, &first, &end));
            }
        }
    }
}

/* ---- Large values and fallbacks ---- */

ZTEST(te_frame, test_frame_large_values) {
    /* A scan of 4000 s, 64 small rects, microseconds near the top of what 32 bits hold */
    uint16_t y0[MAXR];
    uint16_t y1[MAXR];
    uint32_t wmin[MAXR];
    uint32_t wmax[MAXR];
    struct PlanBuf pb;

    for(unsigned i = 0; i < MAXR; i++) {
        y0[i] = (uint16_t)(i * 7U);
        y1[i] = (uint16_t)(i * 7U + 6U);
        wmin[i] = 20000000U;
        wmax[i] = 21000000U;
    }

    struct co5300_te_frame_in in = {
        .age_us = 100000000U,
        .s_us = 4000000000U,
        .tvdl_us = 3900000000U,
        .panel_h = 466,
        .margin_us = 500,
        .gap_us = 50,
        .min_scan = 0,
        .valid = true,
        .n = MAXR,
        .y0 = y0,
        .y1 = y1,
        .w_min_us = wmin,
        .w_max_us = wmax,
    };
    struct co5300_te_frame_scratch sc = {
        .a_us = pb.a, .b_us = pb.b, .best = pb.best, .stack = pb.stack, .stack_t_us = pb.st
    };

    zassert_equal(co5300_te_frame_plan(&in, &sc, pb.order, &pb.scan, &pb.nodes), CO5300_TE_FRAME_OK);
    zassert_true(is_permutation(pb.order, MAXR));
    zassert_true(pb.scan >= 0 && pb.scan <= 3);
    zassert_equal(pb.nodes, 0U);

    /* the schedule, with 64-bit times beyond 2^32 */
    int64_t t = in.age_us;

    for(unsigned k = 0; k < MAXR; k++) {
        uint8_t i = pb.order[k];
        int64_t lo = 0;
        int64_t hi = 0;

        co5300_te_frame_window(pb.a[i], pb.b[i], in.s_us, pb.scan, &lo, &hi);
        t = (t > lo ? t : lo);
        zassert_true(t < hi, "rect %u", k);
        t += (int64_t)wmax[i] + 50;
    }
    zassert_true(t > (1LL << 32) || pb.scan <= 1);

    /* a step, the bound and the audit with such times */
    struct co5300_te_frame_run run = { .ref_us = 5000000000LL, .scan = pb.scan, .epoch = 1, .k = 0 };
    struct co5300_te_frame_snap sn = {
        .now_us = 5000000000LL + 100000000LL,
        .fall_us = 5000000000LL,
        .s_us = in.s_us,
        .tvdl_us = in.tvdl_us,
        .epoch = 1,
        .valid = true,
    };
    uint32_t wait = 0;
    enum co5300_te_frame_action act = co5300_te_frame_step(&run, pb.a, pb.b, pb.order, &sn, &wait);

    zassert_true(act == CO5300_TE_FRAME_WAIT || act == CO5300_TE_FRAME_START || act == CO5300_TE_FRAME_WAIT_EDGE);
    zassert_true(wait <= in.s_us);

    int64_t st[MAXR];
    int64_t en[MAXR];

    for(unsigned i = 0; i < MAXR; i++) {
        st[i] = 5000000000LL + (int64_t)pb.scan * in.s_us + 1000 + (int64_t)i * 22000000LL;
        en[i] = st[i] + 21000000LL;
    }

    struct co5300_te_frame_verdict v = co5300_te_frame_audit(
        MAXR, y0, y1, st, en, wmax, 5000000000LL, in.s_us, in.tvdl_us, 466, 500
    );
    struct co5300_te_frame_bound b = co5300_te_frame_finish(
        MAXR, y0, y1, st, en, 5000000000LL, in.s_us, in.tvdl_us, 466, 500, true, 1
    );

    zassert_false(v.late);
    zassert_equal(v.shown_us, b.at_us);
    zassert_equal(
        co5300_te_frame_min_scan(&b, 5000000000LL, in.s_us, 1), (int32_t)((b.at_us - 5000000000LL) / in.s_us)
    );
    b.at_us = INT64_MAX / 4;
    zassert_equal(co5300_te_frame_min_scan(&b, 0, 1, 1), INT32_MAX);

    /* several seconds, rows near the bottom, a long write */
    in.s_us = 3000000U;
    in.tvdl_us = 2900000U;
    in.age_us = 3200000U;
    in.n = 1;
    y0[0] = 400;
    y1[0] = 465;
    wmin[0] = 800000U;
    wmax[0] = 900000U;
    zassert_equal(co5300_te_frame_plan(&in, &sc, pb.order, &pb.scan, &pb.nodes), CO5300_TE_FRAME_OK);

    /* an age near the top of 32 bits is stale, with no overflow in the check */
    in.age_us = 4294967295U;
    zassert_equal(co5300_te_frame_plan(&in, &sc, pb.order, &pb.scan, &pb.nodes), CO5300_TE_FRAME_FALLBACK);

    /* a huge N_min does not overflow either */
    in.age_us = 100;
    in.min_scan = INT32_MAX;
    zassert_equal(co5300_te_frame_plan(&in, &sc, pb.order, &pb.scan, &pb.nodes), CO5300_TE_FRAME_OK);
    zassert_equal(pb.scan, INT32_MAX);
}

ZTEST(te_frame, test_frame_fallbacks) {
    struct Frame f;
    struct PlanBuf pb;

    f = (struct Frame){ 0 };
    add_rect(&f, 100, 150, 100);
    set_bounds(&f, 100, 300);

    zassert_equal(plan_frame(&pb, &f, 5000, 0, MARGIN, GAP, true), CO5300_TE_FRAME_OK);
    zassert_equal(plan_frame(&pb, &f, 5000, 0, MARGIN, GAP, false), CO5300_TE_FRAME_FALLBACK);

    /* stale: older than S + S/10 */
    zassert_equal(plan_frame(&pb, &f, S_US + S_US / 10U, 0, MARGIN, GAP, true), CO5300_TE_FRAME_OK);
    zassert_equal(plan_frame(&pb, &f, S_US + S_US / 10U + 1U, 0, MARGIN, GAP, true), CO5300_TE_FRAME_FALLBACK);

    /* nonsense input */
    f.y1[0] = H;
    zassert_equal(plan_frame(&pb, &f, 5000, 0, MARGIN, GAP, true), CO5300_TE_FRAME_FALLBACK);
    f.y0[0] = 150;
    f.y1[0] = 100;
    zassert_equal(plan_frame(&pb, &f, 5000, 0, MARGIN, GAP, true), CO5300_TE_FRAME_FALLBACK);
    f.y0[0] = 100;
    f.y1[0] = 150;
    f.wmin[0] = f.wmax[0] + 1U;
    zassert_equal(plan_frame(&pb, &f, 5000, 0, MARGIN, GAP, true), CO5300_TE_FRAME_FALLBACK);
    set_bounds(&f, 100, 300);

    struct co5300_te_frame_in in = {
        .age_us = 5000,
        .s_us = S_US,
        .tvdl_us = TVDL_US,
        .panel_h = H,
        .margin_us = MARGIN,
        .gap_us = GAP,
        .valid = true,
        .n = 1,
        .y0 = f.y0,
        .y1 = f.y1,
        .w_min_us = f.wmin,
        .w_max_us = f.wmax,
    };
    struct co5300_te_frame_scratch sc = {
        .a_us = pb.a, .b_us = pb.b, .best = pb.best, .stack = pb.stack, .stack_t_us = pb.st
    };

    zassert_equal(co5300_te_frame_plan(&in, &sc, pb.order, &pb.scan, &pb.nodes), CO5300_TE_FRAME_OK);
    in.n = 0;
    zassert_equal(co5300_te_frame_plan(&in, &sc, pb.order, &pb.scan, &pb.nodes), CO5300_TE_FRAME_FALLBACK);
    in.n = 1;
    in.s_us = 0;
    zassert_equal(co5300_te_frame_plan(&in, &sc, pb.order, &pb.scan, &pb.nodes), CO5300_TE_FRAME_FALLBACK);
    in.s_us = S_US;
    in.tvdl_us = S_US + 1U;
    zassert_equal(co5300_te_frame_plan(&in, &sc, pb.order, &pb.scan, &pb.nodes), CO5300_TE_FRAME_FALLBACK);
    in.tvdl_us = 0;
    zassert_equal(co5300_te_frame_plan(&in, &sc, pb.order, &pb.scan, &pb.nodes), CO5300_TE_FRAME_FALLBACK);
    in.tvdl_us = TVDL_US;
    in.panel_h = 0;
    zassert_equal(co5300_te_frame_plan(&in, &sc, pb.order, &pb.scan, &pb.nodes), CO5300_TE_FRAME_FALLBACK);
}

/* The budget is per frame, not per target scan: 5-6 rects at ages past one scan try several scans (plan_frame asserts it). */
ZTEST(te_frame, test_search_budget_per_frame) {
    struct Frame f;
    struct PlanBuf pb;
    unsigned many = 0;

    seed_rng(0x5b0d9e7);
    for(unsigned c = 0; c < 40000; c++) {
        random_frame(&f, 5, 6);
        set_bounds(&f, 100, 300);
        for(unsigned i = 0; i < f.n; i++) {
            if(rnd_u32() % 2 == 0) {
                f.x[i] = H;
            }
        }
        set_bounds(&f, 100, 300);

        uint32_t age = (uint32_t)rnd_in(0, S_US + S_US / 10U);

        (void)plan_frame(&pb, &f, age, (int32_t)(rnd_u32() % 2), MARGIN, GAP, true);
        if(pb.nodes > 1000) {
            many++;
        }
    }
    zassert_true(many > 0, "no frame searched more than 1000 nodes: the sweep does not reach the budget");
}

ZTEST_SUITE(te_frame, NULL, NULL, NULL, NULL, NULL);
