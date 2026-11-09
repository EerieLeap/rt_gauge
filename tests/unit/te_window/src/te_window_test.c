/*
 * Copyright (c) 2025 Pavel Maloletkov.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/ztest.h>

#include "co5300_te_window.h"

/* The numbers measured on the unit (2026-11-09). */
enum {
    S_US = 16800U,
    TVDL_US = 16190U,
    H = 466U,
    MARGIN = 500U,
};

static uint32_t row_t(uint16_t y) {
    return co5300_te_row_time(TVDL_US, H, y);
}

static struct co5300_te_window_in
make_in(uint32_t age, uint16_t y0, uint16_t y1, uint32_t w_min, uint32_t w_max, uint32_t margin) {
    struct co5300_te_window_in in = {
        .age_us = age,
        .s_us = S_US,
        .tvdl_us = TVDL_US,
        .panel_h = H,
        .y0 = y0,
        .y1 = y1,
        .w_min_us = w_min,
        .w_max_us = w_max,
        .margin_us = margin,
        .valid = true,
    };

    return in;
}

/* ---- The simulation the property tests check against ---- */

/* A scan k sees row y of the area new when the writer wrote it before the scan read it. The writer
 * writes row i of h rows at start + (i + 1) / h * w (finished_at_row_end) or, exactly linear from the
 * first row at the start to the last at the end, at start + i / (h - 1) * w. Times are scaled by
 * the denominator to stay in integers. Ties count as old (the scan was first). */
static bool sim_torn(int64_t start, int64_t w, int64_t s, uint32_t tvdl, uint16_t y0, uint16_t y1, bool exact_linear) {
    int64_t h = (int64_t)y1 - y0 + 1;
    int64_t den = h;

    if(exact_linear) {
        den = h > 1 ? h - 1 : 1;
    }

    for(int64_t k = 0; k <= (start + w) / s + 1; k++) {
        bool any_new = false;
        bool any_old = false;

        for(int64_t y = y0; y <= y1; y++) {
            int64_t i = y - y0 + (exact_linear ? 0 : 1);
            int64_t written = start * den + i * w;
            int64_t read = (k * s + co5300_te_row_time(tvdl, H, (uint16_t)y)) * den;

            if(written < read) {
                any_new = true;
            } else {
                any_old = true;
            }
        }
        if(any_new && any_old) {
            return true;
        }
    }
    return false;
}

/* ---- Row times and decisions ---- */

ZTEST(te_window, test_row_time) {
    zassert_equal(co5300_te_row_time(TVDL_US, H, 0), 0);
    zassert_equal(co5300_te_row_time(TVDL_US, H, 465), (uint32_t)(465ULL * 16190 / 466));
    zassert_equal(co5300_te_row_time(TVDL_US, H, 233), 233ULL * 16190 / 466);
    /* y * tvdl needs 64 bits */
    zassert_equal(co5300_te_row_time(3900000000U, 466, 465), (uint32_t)(465ULL * 3900000000ULL / 466));
    zassert_equal(co5300_te_row_time(TVDL_US, 0, 5), 0);
}

ZTEST(te_window, test_full_frame_windows) {
    /* t0 = 0, t1 = 16155; a = 16155 - 23900 - 500 = -8245, b = 0 + 500 + max(0, 16155 - 23500) = 500 */
    zassert_equal(row_t(465), 16155U);

    /* Allowed: after the margin, until 8245 us before the next edge */
    for(uint32_t age = 500; age < S_US - 8245U; age += 250) {
        struct co5300_te_window_in in = make_in(age, 0, 465, 23500, 23900, MARGIN);

        zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_START_NOW, "age %u", age);
    }

    /* Exactly at the end of the interval */
    struct co5300_te_window_in in = make_in(500, 0, 465, 23500, 23900, MARGIN);
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_START_NOW);

    /* Just before its start */
    in.age_us = S_US - 8245U - 1U;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_START_NOW);

    /* At its start: the whole interval is waited out, across the edge */
    in.age_us = S_US - 8245U;
    struct co5300_te_decision d = co5300_te_window_decide(&in);
    zassert_equal(d.action, CO5300_TE_WAIT_EDGE);
    zassert_equal(d.wait_us, 8245U);

    /* Inside, before the edge: the wait crosses it */
    in.age_us = 9000;
    d = co5300_te_window_decide(&in);
    zassert_equal(d.action, CO5300_TE_WAIT_EDGE);
    zassert_equal(d.wait_us, S_US - 9000U);

    /* Just after the edge: a plain wait to the interval's end */
    in.age_us = 100;
    d = co5300_te_window_decide(&in);
    zassert_equal(d.action, CO5300_TE_WAIT);
    zassert_equal(d.wait_us, 400U);

    in.age_us = 0;
    d = co5300_te_window_decide(&in);
    zassert_equal(d.action, CO5300_TE_WAIT);
    zassert_equal(d.wait_us, 500U);

    /* An age past one period is taken modulo S (the edge is stale only past S + S/10) */
    in.age_us = S_US + 100U;
    d = co5300_te_window_decide(&in);
    zassert_equal(d.action, CO5300_TE_WAIT);
    zassert_equal(d.wait_us, 400U);
}

ZTEST(te_window, test_band_above_and_below_the_scanner) {
    /* A band at the top: the scanner reaches its first row at once; slower writer. */
    uint16_t y0 = 0;
    uint16_t y1 = 99;
    uint32_t w = 5193;
    struct co5300_te_window_in in = make_in(0, y0, y1, w, w, 0);

    /* forbidden (t1 - w, t0) = (3440 - 5193, 0): the last 1753 us before the edge */
    uint32_t t1 = row_t(y1);

    in.age_us = S_US - (w - t1) - 1U;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_START_NOW);
    in.age_us = S_US - (w - t1);
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_WAIT_EDGE);
    in.age_us = 0;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_START_NOW, "the interval's end is allowed");

    /* A band low on the screen: the forbidden interval lies late in the scan */
    y0 = 300;
    y1 = 399;
    in = make_in(0, y0, y1, w, w, 0);
    uint32_t t0 = row_t(y0);

    t1 = row_t(y1);
    zassert_true(w > t1 - t0, "slow writer");
    /* forbidden (t1 - w, t0) */
    in.age_us = t1 - w + 1U;
    struct co5300_te_decision d = co5300_te_window_decide(&in);

    zassert_equal(d.action, CO5300_TE_WAIT);
    zassert_equal(d.wait_us, t0 - (t1 - w + 1U));
    in.age_us = t0;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_START_NOW);
    in.age_us = t1 - w;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_WAIT, "the start of the interval is forbidden");
}

ZTEST(te_window, test_fast_writer_never_waits_for_an_edge) {
    /* 200 x 120 area: 2817 us against 4134 us of scan */
    uint16_t y0 = 200;
    uint16_t y1 = 319;
    uint32_t w = 2817;
    uint32_t t0 = row_t(y0);
    uint32_t t1 = row_t(y1);

    zassert_true(w < t1 - t0, "fast writer");

    /* forbidden (t0, t1 - w) */
    for(uint32_t age = 0; age < S_US; age += 10) {
        struct co5300_te_window_in in = make_in(age, y0, y1, w, w, 0);
        struct co5300_te_decision d = co5300_te_window_decide(&in);

        zassert_not_equal(d.action, CO5300_TE_WAIT_EDGE, "age %u", age);
        zassert_not_equal(d.action, CO5300_TE_FALLBACK);
        if(age >= t0 && age < t1 - w) {
            zassert_equal(d.action, CO5300_TE_WAIT, "age %u", age);
            zassert_equal(d.wait_us, t1 - w - age);
        } else {
            zassert_equal(d.action, CO5300_TE_START_NOW, "age %u", age);
        }
    }
}

ZTEST(te_window, test_near_equal_speeds_union) {
    /* 100..299: 200 rows; the scan covers t1 - t0 in 6919 us */
    uint16_t y0 = 100;
    uint16_t y1 = 299;
    uint32_t t0 = row_t(y0);
    uint32_t t1 = row_t(y1);
    uint32_t eq = t1 - t0;
    /* w_min below and w_max above the scan's own time: the union (t1 - w_max, t1 - w_min) around t0.. */
    struct co5300_te_window_in in = make_in(0, y0, y1, eq - 100, eq + 100, 0);

    /* a = min(t1 - w_max, t0) = t0 - 100, b = max(t0, t1 - w_min) = t0 + 100 */
    in.age_us = t0 - 101U;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_START_NOW);
    in.age_us = t0 - 100U;
    struct co5300_te_decision d = co5300_te_window_decide(&in);

    zassert_equal(d.action, CO5300_TE_WAIT);
    zassert_equal(d.wait_us, 200U);
    in.age_us = t0 + 99U;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_WAIT);
    in.age_us = t0 + 100U;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_START_NOW);
}

ZTEST(te_window, test_margin_widens_both_sides) {
    uint16_t y0 = 200;
    uint16_t y1 = 319;
    uint32_t w = 2817;
    uint32_t t0 = row_t(y0);
    uint32_t t1 = row_t(y1);
    struct co5300_te_window_in in = make_in(t0 - 301U, y0, y1, w, w, 300);

    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_START_NOW);
    in.age_us = t0 - 300U;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_WAIT);
    in.age_us = t1 - w + 299U;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_WAIT);
    in.age_us = t1 - w + 300U;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_START_NOW);
}

ZTEST(te_window, test_fallbacks) {
    struct co5300_te_window_in in = make_in(5000, 0, 465, 23500, 23900, MARGIN);

    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_START_NOW);

    in.valid = false;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_FALLBACK);
    in.valid = true;

    /* stale: older than S + S/10 */
    in.age_us = S_US + S_US / 10U;
    zassert_not_equal(co5300_te_window_decide(&in).action, CO5300_TE_FALLBACK);
    in.age_us = S_US + S_US / 10U + 1U;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_FALLBACK);
    in.age_us = 5000;

    /* no tear-free start: the forbidden interval covers the scan */
    in.margin_us = 5000;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_FALLBACK);
    in.margin_us = MARGIN;
    in.w_max_us = 40000;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_FALLBACK);
    in.w_max_us = 23900;

    /* nonsense input */
    in.y1 = 466;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_FALLBACK);
    in.y1 = 465;
    in.y0 = 10;
    in.y1 = 9;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_FALLBACK);
    in = make_in(5000, 0, 465, 23500, 23900, MARGIN);
    in.s_us = 0;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_FALLBACK);
    in = make_in(5000, 0, 465, 23900, 23500, MARGIN);
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_FALLBACK);
    in = make_in(5000, 0, 465, 23500, 23900, MARGIN);
    in.tvdl_us = S_US + 1U;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_FALLBACK);
}

ZTEST(te_window, test_large_values) {
    /* A scan of 4000 s, microseconds near the top of what 32 bits hold */
    struct co5300_te_window_in in = {
        .age_us = 100000000U,
        .s_us = 4000000000U,
        .tvdl_us = 3900000000U,
        .panel_h = 466,
        .y0 = 0,
        .y1 = 465,
        .w_min_us = 2000000000U,
        .w_max_us = 2100000000U,
        .margin_us = 500,
        .valid = true,
    };
    struct co5300_te_decision d = co5300_te_window_decide(&in);

    /* t1 = 3891630901; a = min(3891630901 - 2.1e9, 0) - 500 = -500: b = 3891630901 - 2e9 + 500 */
    uint64_t t1 = 465ULL * 3900000000ULL / 466;
    uint64_t b = t1 - 2000000000ULL + 500ULL;

    zassert_equal(d.action, CO5300_TE_WAIT);
    zassert_equal(d.wait_us, (uint32_t)(b - 100000000ULL), "wait %u", d.wait_us);

    /* The age equals the period: phase 0, which lies inside the interval too */
    in.age_us = 4000000000U;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_WAIT);

    /* age near the top of 32 bits, with a stale edge: no overflow in the staleness check */
    in.s_us = 3000000000U;
    in.tvdl_us = 2900000000U;
    in.age_us = 4294967295U;
    zassert_equal(co5300_te_window_decide(&in).action, CO5300_TE_FALLBACK);

    /* Several seconds, age within the limit, rows near the bottom */
    in.s_us = 3000000U;
    in.tvdl_us = 2900000U;
    in.age_us = 3200000U;
    in.y0 = 400;
    in.y1 = 465;
    in.w_min_us = 800000U;
    in.w_max_us = 900000U;
    d = co5300_te_window_decide(&in);
    zassert_not_equal(d.action, CO5300_TE_FALLBACK);
    zassert_true(d.wait_us <= in.s_us);
}

/* ---- The audit ---- */

ZTEST(te_window, test_audit_late_counts_a_stalled_writer) {
    uint32_t t0 = row_t(150);
    uint32_t t1 = row_t(315);
    uint32_t w_max = 3170U;
    uint32_t start = 4710U;

    /* The straight line from the first pixel is clean, and the rule's bound holds: not late. */
    zassert_false(co5300_te_audit_torn(start, w_max, S_US, t0, t1));
    zassert_false(co5300_te_audit_late(start, w_max, S_US, t0, t1, w_max, MARGIN));
    /* A stall of 2455 us at the piece gap: both end rows are new, the rows between are old, and the
     * two-row check is blind to it; the length gives it away. */
    zassert_false(co5300_te_audit_torn(start, w_max + 2455U, S_US, t0, t1));
    zassert_true(co5300_te_audit_late(start, w_max + 2455U, S_US, t0, t1, w_max, MARGIN));
    /* Within the margin is not late. */
    zassert_false(co5300_te_audit_late(start, w_max + MARGIN, S_US, t0, t1, w_max, MARGIN));
    zassert_true(co5300_te_audit_late(start, w_max + MARGIN + 1U, S_US, t0, t1, w_max, MARGIN));
    /* A straight-line tear is still reported. */
    zassert_true(co5300_te_audit_late(S_US - 5000U, 23487U, S_US, row_t(0), row_t(465), 23487U, MARGIN));
    zassert_false(co5300_te_audit_late(0, 2817, 0, t0, t1, 3000U, MARGIN));
}

ZTEST(te_window, test_audit_examples) {
    uint32_t t0 = row_t(0);
    uint32_t t1 = row_t(465);
    uint32_t w = 23487;

    /* Start right after the edge: the scan 0 passes below the writer, no tear. */
    zassert_false(co5300_te_audit_torn(600, w, S_US, t0, t1));
    /* The window's far end: 8245 us before the edge */
    zassert_false(co5300_te_audit_torn(S_US - 8245U - 1U, w, S_US, t0, t1));
    /* Inside the forbidden interval: the next scan catches up with the writer */
    zassert_true(co5300_te_audit_torn(S_US - 5000U, w, S_US, t0, t1));
    zassert_true(co5300_te_audit_torn(S_US - 100U, w, S_US, t0, t1));
    /* Meeting exactly is no tear */
    zassert_false(co5300_te_audit_torn(S_US - (w - t1), w, S_US, t0, t1));
    /* A fast writer inside the interval (t0, t1 - w) */
    t0 = row_t(200);
    t1 = row_t(319);
    zassert_true(co5300_te_audit_torn(t0 + 100U, 2817, S_US, t0, t1));
    zassert_false(co5300_te_audit_torn(t0 - 100U, 2817, S_US, t0, t1));
    zassert_false(co5300_te_audit_torn(t1 - 2817U + 1U, 2817, S_US, t0, t1));
    zassert_false(co5300_te_audit_torn(0, 2817, 0, t0, t1));
}

/* Cases: y0, y1, write time (off the 50 us grid, so no start phase of the sweeps ties) */
struct AreaCase {
    uint16_t y0;
    uint16_t y1;
    uint32_t w;
};

static const struct AreaCase CLEAR_CASES[] = {
    { 0, 465, 23487 }, /* a full frame */
    { 0, 465, 23911 },
    { 0, 99, 5193 },    /* a band at the top */
    { 300, 399, 5193 }, /* a band below the middle */
    { 400, 465, 3607 }, /* the bottom band */
    { 200, 319, 2817 }, /* a digits box: fast */
    { 210, 249, 461 },  /* a small box: fast */
    { 440, 465, 331 },
    { 0, 465, 17713 }, /* a frame that is 1 ms slower than the scan */
};

ZTEST(te_window, test_audit_agrees_with_the_simulation) {
    int checked = 0;

    for(size_t c = 0; c < ARRAY_SIZE(CLEAR_CASES); c++) {
        const struct AreaCase* a = &CLEAR_CASES[c];
        uint32_t t0 = row_t(a->y0);
        uint32_t t1 = row_t(a->y1);

        for(uint32_t phase = 7; phase < S_US; phase += 37) {
            bool audit = co5300_te_audit_torn(phase, a->w, S_US, t0, t1);
            bool sim = sim_torn(phase, a->w, S_US, TVDL_US, a->y0, a->y1, true);

            zassert_equal(audit, sim, "case %u phase %u: audit %d sim %d", (unsigned)c, phase, audit, sim);
            checked++;
        }
    }
    zassert_true(checked > 1000);
}

/* ---- The key property: no start the rule allows tears ---- */

/* Follows decide's waits from a start phase the way the driver does: a WAIT advances the age, a
 * WAIT_EDGE reaches the edge (age 0). Returns the phase of the start, or -1 for no start in 6 rounds. */
static int64_t follow_starts(struct co5300_te_window_in in, uint32_t phase, unsigned* rounds) {
    uint64_t age = phase;

    for(unsigned i = 0; i < 6; i++) {
        in.age_us = (uint32_t)age;
        struct co5300_te_decision d = co5300_te_window_decide(&in);

        *rounds = i;
        switch(d.action) {
            case CO5300_TE_START_NOW:
                return (int64_t)(age % in.s_us);
            case CO5300_TE_WAIT:
                age += d.wait_us;
                break;
            case CO5300_TE_WAIT_EDGE:
                zassert_true(age % in.s_us + d.wait_us >= in.s_us, "an edge wait must cross the edge");
                age = 0;
                break;
            default:
                return -1;
        }
    }
    return -1;
}

static void check_case(const struct AreaCase* a, uint32_t margin, uint32_t spread) {
    /* The bounds bracket the real write time; the real one is the lower, the upper and the middle */
    uint32_t w_min = a->w - spread;
    uint32_t w_max = a->w + spread;
    uint32_t reals[3] = { w_min, a->w, w_max };
    struct co5300_te_window_in in = make_in(0, a->y0, a->y1, w_min, w_max, margin);

    for(uint32_t phase = 0; phase < S_US; phase += 50) {
        in.age_us = phase;

        struct co5300_te_decision d = co5300_te_window_decide(&in);
        unsigned rounds = 0;
        int64_t start = follow_starts(in, phase, &rounds);

        zassert_true(d.action != CO5300_TE_FALLBACK);
        zassert_true(
            start >= 0 && rounds <= 2, "case %u-%u w %u phase %u: no start (rounds %u)", a->y0, a->y1, a->w, phase,
            rounds
        );
        for(int r = 0; r < 3; r++) {
            zassert_false(
                sim_torn(start, reals[r], S_US, TVDL_US, a->y0, a->y1, false),
                "case %u-%u w %u real %u phase %u margin %u: torn at start %d", a->y0, a->y1, a->w, reals[r], phase,
                margin, (int)start
            );
        }
        if(d.action == CO5300_TE_START_NOW) {
            zassert_equal(start, phase, "START_NOW follows to itself");
        }
    }
}

ZTEST(te_window, test_no_allowed_start_tears_margin_zero) {
    for(size_t c = 0; c < ARRAY_SIZE(CLEAR_CASES); c++) {
        check_case(&CLEAR_CASES[c], 0, 0);
    }
}

ZTEST(te_window, test_no_allowed_start_tears_with_margin_and_bounds) {
    for(size_t c = 0; c < ARRAY_SIZE(CLEAR_CASES); c++) {
        check_case(&CLEAR_CASES[c], MARGIN, 0);
        check_case(&CLEAR_CASES[c], MARGIN, 150);
        check_case(&CLEAR_CASES[c], 0, 123);
    }
}

ZTEST(te_window, test_no_allowed_start_tears_near_equal_speeds) {
    /* Writer and scan about equally fast: only a margin of about a row (35 us) makes it safe */
    static const struct AreaCase NEAR_EQUAL_CASES[] = {
        { 100, 299, 6913 },
        { 100, 299, 6951 },
        { 100, 299, 7003 },
        { 0, 465, 16119 },
        { 0, 465, 16311 },
        { 300, 465, 5719 },
    };

    for(size_t c = 0; c < ARRAY_SIZE(NEAR_EQUAL_CASES); c++) {
        check_case(&NEAR_EQUAL_CASES[c], MARGIN, 0);
        check_case(&NEAR_EQUAL_CASES[c], MARGIN, 90);
    }
}

ZTEST(te_window, test_allowed_starts_exist_for_the_frame) {
    /* A full frame may start in about half of the scan: 8.2 ms of 16.8 */
    struct co5300_te_window_in in = make_in(0, 0, 465, 23500, 23900, MARGIN);
    unsigned allowed = 0;

    for(uint32_t phase = 0; phase < S_US; phase += 10) {
        in.age_us = phase;
        if(co5300_te_window_decide(&in).action == CO5300_TE_START_NOW) {
            allowed++;
        }
    }
    zassert_within(allowed * 10U, 16800U - 8745U, 20U);
}

/* ---- Write time bounds ---- */

ZTEST(te_window, test_model_and_classes) {
    zassert_equal(co5300_te_ns_per_byte(40000000), 50U);
    zassert_equal(co5300_te_ns_per_byte(20000000), 100U);
    zassert_equal(co5300_te_ns_per_byte(0), 50U);

    /* 466 x 466 x 2 = 434312 B: 300 + 21715 + 13 * 115 */
    zassert_equal(co5300_te_w_model_us(434312, 50), 300U + 21715U + 1495U);
    zassert_equal(co5300_te_w_model_us(32768, 50), 300U + 1638U);
    zassert_equal(co5300_te_w_model_us(32769, 50), 300U + 1638U + 115U);
    zassert_equal(co5300_te_w_model_us(1, 50), 300U);

    zassert_equal(co5300_te_w_class(0), 0U);
    zassert_equal(co5300_te_w_class(16383), 0U);
    zassert_equal(co5300_te_w_class(16384), 1U);
    zassert_equal(co5300_te_w_class(131071), 1U);
    zassert_equal(co5300_te_w_class(131072), 2U);
    zassert_equal(co5300_te_w_class(434312), 2U);
}

ZTEST(te_window, test_bounds_before_any_sample) {
    struct co5300_te_wbounds wb;
    uint32_t lo = 0;
    uint32_t hi = 0;

    co5300_te_wbounds_init(&wb);
    co5300_te_wbounds_get(&wb, 434312, 50, &lo, &hi);
    zassert_equal(hi, 23510U + 600U);
    zassert_equal(lo, 23510U - 150U);

    /* Never zero */
    co5300_te_wbounds_get(&wb, 1, 50, &lo, &hi);
    zassert_equal(lo, 300U - 150U);
    co5300_te_wbounds_get(&wb, 1, 0, &lo, &hi);
    zassert_true(lo >= 1U);
}

ZTEST(te_window, test_bounds_seed_follow_and_decay) {
    struct co5300_te_wbounds wb;
    uint32_t lo = 0;
    uint32_t hi = 0;
    uint32_t model = co5300_te_w_model_us(434312, 50);

    co5300_te_wbounds_init(&wb);

    /* The first sample seeds both envelopes */
    co5300_te_wbounds_update(&wb, 434312, 50, model + 100);
    co5300_te_wbounds_get(&wb, 434312, 50, &lo, &hi);
    zassert_equal(lo, model + 100U);
    zassert_equal(hi, model + 100U);

    /* A higher sample: the maximum follows at once, the minimum creeps up by 1/128 */
    co5300_te_wbounds_update(&wb, 434312, 50, model + 1100);
    co5300_te_wbounds_get(&wb, 434312, 50, &lo, &hi);
    zassert_equal(hi, model + 1100U);
    zassert_equal(lo, model + 100U + 1000U / 128U);

    /* A lower sample: the minimum follows at once, the maximum decays by (distance / 128) */
    co5300_te_wbounds_update(&wb, 434312, 50, model - 900);
    co5300_te_wbounds_get(&wb, 434312, 50, &lo, &hi);
    zassert_equal(lo, model - 900U);
    zassert_equal(hi, model + 1100U - 2000U / 128U);

    /* The decay is at least 1 us and never passes the sample */
    co5300_te_wbounds_init(&wb);
    co5300_te_wbounds_update(&wb, 434312, 50, model + 10);
    co5300_te_wbounds_update(&wb, 434312, 50, model + 40); /* max 40, min 10 */
    for(int i = 0; i < 100; i++) {
        co5300_te_wbounds_update(&wb, 434312, 50, model + 20);
    }
    co5300_te_wbounds_get(&wb, 434312, 50, &lo, &hi);
    zassert_equal(hi, model + 20U, "max decays to the sample in 1 us steps");
    zassert_equal(lo, model + 20U, "the minimum creeps up to the sample in 1 us steps");
}

ZTEST(te_window, test_bounds_min_mirror) {
    struct co5300_te_wbounds wb;
    uint32_t lo = 0;
    uint32_t hi = 0;
    uint32_t model = co5300_te_w_model_us(434312, 50);

    co5300_te_wbounds_init(&wb);
    co5300_te_wbounds_update(&wb, 434312, 50, model - 500);  /* env_min = env_max = -500 */
    co5300_te_wbounds_update(&wb, 434312, 50, model + 1500); /* max +1500, min -500 + 2000/128 */
    co5300_te_wbounds_get(&wb, 434312, 50, &lo, &hi);
    zassert_equal(hi, model + 1500U);
    zassert_equal(lo, model - 500U + 2000U / 128U);
}

ZTEST(te_window, test_bounds_classes_are_independent) {
    struct co5300_te_wbounds wb;
    uint32_t lo = 0;
    uint32_t hi = 0;

    co5300_te_wbounds_init(&wb);
    co5300_te_wbounds_update(&wb, 434312, 50, co5300_te_w_model_us(434312, 50) + 77);
    co5300_te_wbounds_get(&wb, 48000, 50, &lo, &hi);
    zassert_equal(hi, co5300_te_w_model_us(48000, 50) + 600U, "the large class has no sample");
    co5300_te_wbounds_get(&wb, 434312, 50, &lo, &hi);
    zassert_equal(hi, co5300_te_w_model_us(434312, 50) + 77U);
    co5300_te_wbounds_get(&wb, 3200, 50, &lo, &hi);
    zassert_equal(lo, co5300_te_w_model_us(3200, 50) - 150U);
}

ZTEST(te_window, test_bounds_cover_samples_and_keep_order) {
    struct co5300_te_wbounds wb;
    uint32_t lo = 0;
    uint32_t hi = 0;
    uint32_t model = co5300_te_w_model_us(434312, 50);

    co5300_te_wbounds_init(&wb);
    for(uint32_t i = 0; i < 2000; i++) {
        uint32_t w = model - 100U + (i * 7919U) % 400U;

        co5300_te_wbounds_update(&wb, 434312, 50, w);
        co5300_te_wbounds_get(&wb, 434312, 50, &lo, &hi);
        zassert_true(lo <= w && w <= hi, "sample %u not within [%u, %u]", w, lo, hi);
        zassert_true(lo <= hi);
    }
}

ZTEST_SUITE(te_window, NULL, NULL, NULL, NULL, NULL);
