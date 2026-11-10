#ifdef CONFIG_EERIE_LEAP_UI_BENCH

    #include <atomic>
    #include <cmath>
    #include <cstdint>
    #include <cstdio>
    #include <cstdlib>
    #include <cstring>

    #include <lvgl.h>
    #include <lvgl_private.h>
    #include <zephyr/device.h>
    #include <zephyr/kernel.h>
    #include <zephyr/shell/shell.h>
    #include <zephyr/sys/printk.h>

    #ifdef CONFIG_ELCO5300_STATS
        #include <zephyr/drivers/display/display_co5300.h>
    #endif
    #ifdef CONFIG_MSPI_ESP32_STATS
        #include <zephyr/drivers/mspi/mspi_esp32.h>
    #endif

    #include "domain/ui_domain/lvgl_lock.h"
    #include "utilities/dev_tools/system_info.h"

    #include "ui_bench.h"

    #ifdef CONFIG_LV_Z_FLUSH_THREAD
extern "C" {
extern const k_tid_t lvgl_flush_thread;
}
    #endif

namespace eerie_leap::utilities::dev_tools {

using eerie_leap::domain::ui_domain::ScopedLvglLock;

namespace {

// Written by the display event callbacks on the renderer thread, read by the shell thread.
struct Counters {
    uint32_t refreshes = 0;
    uint64_t refr_cycles = 0;
    uint32_t refr_cycles_max = 0;
    uint32_t areas = 0;
    uint64_t wait_cycles = 0;
    uint32_t wait_cycles_max = 0;
};

struct Baseline {
    int64_t uptime_ms = 0;
    uint64_t renderer_cycles = 0;
    uint64_t flush_cycles = 0;
    uint64_t idle_cycles = 0;
    uint64_t busy_cycles = 0;
};

k_spinlock lock_;
Counters counters_;
Baseline baseline_;
uint32_t refr_start_ = 0;
uint32_t wait_start_ = 0;

[[maybe_unused]] const device* display_dev_ = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    #ifdef CONFIG_MSPI_ESP32_STATS
const device* mspi_dev_ = DEVICE_DT_GET(DT_BUS(DT_CHOSEN(zephyr_display)));
    #endif

std::atomic<bool> full_ = false;
std::atomic<k_tid_t> renderer_tid_ = nullptr;
int renderer_priority_ = 0;

uint64_t CyclesToUs(uint64_t cycles) {
    return k_cyc_to_us_floor64(cycles);
}

// Microseconds times 100.
[[maybe_unused]] uint64_t Us100(uint64_t cycles) {
    return k_cyc_to_ns_floor64(cycles) / 10;
}

uint64_t ThreadCycles(k_tid_t tid) {
    #ifdef CONFIG_SCHED_THREAD_USAGE
    if(tid == nullptr)
        return 0;

    k_thread_runtime_stats_t stats;
    if(k_thread_runtime_stats_get(tid, &stats) != 0)
        return 0;

    return stats.execution_cycles;
    #else
    ARG_UNUSED(tid);
    return 0;
    #endif
}

k_tid_t FlushTid() {
    #ifdef CONFIG_LV_Z_FLUSH_THREAD
    return lvgl_flush_thread;
    #else
    return nullptr;
    #endif
}

void ReadAll(uint64_t& idle, uint64_t& busy) {
    idle = 0;
    busy = 0;
    #ifdef CONFIG_SCHED_THREAD_USAGE_ALL
    k_thread_runtime_stats_t stats;
    if(k_thread_runtime_stats_all_get(&stats) == 0) {
        idle = stats.idle_cycles;
        busy = stats.total_cycles;
    }
    #endif
}

void DisplayEvent(lv_event_t* e) {
    const uint32_t now = k_cycle_get_32();
    const lv_event_code_t code = lv_event_get_code(e);

    switch(code) {
        case LV_EVENT_REFR_START: {
            K_SPINLOCK(&lock_) {
                refr_start_ = now;
            }
            break;
        }
        case LV_EVENT_REFR_READY: {
            K_SPINLOCK(&lock_) {
                const uint32_t delta = now - refr_start_;
                ++counters_.refreshes;
                counters_.refr_cycles += delta;
                counters_.refr_cycles_max = MAX(counters_.refr_cycles_max, delta);
            }
            break;
        }
        case LV_EVENT_FLUSH_START: {
            K_SPINLOCK(&lock_) {
                ++counters_.areas;
            }
    #ifdef CONFIG_ELCO5300_STATS
            co5300_stats_mark_post(display_dev_, now, k_thread_priority_get(k_current_get()) < renderer_priority_);
    #endif
            break;
        }
        case LV_EVENT_FLUSH_WAIT_START: {
            K_SPINLOCK(&lock_) {
                wait_start_ = now;
            }
            break;
        }
        case LV_EVENT_FLUSH_WAIT_FINISH: {
            K_SPINLOCK(&lock_) {
                const uint32_t delta = now - wait_start_;
                counters_.wait_cycles += delta;
                counters_.wait_cycles_max = MAX(counters_.wait_cycles_max, delta);
            }
            break;
        }
        default:
            break;
    }
}

// Runs on the renderer thread inside lv_timer_handler(), under the LVGL lock.
void FullTimer(lv_timer_t* timer) {
    ARG_UNUSED(timer);

    renderer_tid_.store(k_current_get(), std::memory_order_relaxed);

    if(full_.load(std::memory_order_relaxed))
        lv_obj_invalidate(lv_screen_active());
}

    #if LV_USE_PROFILER_BUILTIN

std::atomic<bool> trace_on_ = false;

uint64_t ProfilerTick() {
    return k_cyc_to_us_floor64(k_cycle_get_64());
}

void ProfilerFlush(const char* buf) {
    printk("%s", buf);
}

    #endif

void ResetStats() {
    uint64_t idle = 0;
    uint64_t busy = 0;
    ReadAll(idle, busy);
    const uint64_t renderer = ThreadCycles(renderer_tid_.load());
    const uint64_t flush = ThreadCycles(FlushTid());

    K_SPINLOCK(&lock_) {
        counters_ = Counters{};
        baseline_.uptime_ms = k_uptime_get();
        baseline_.renderer_cycles = renderer;
        baseline_.flush_cycles = flush;
        baseline_.idle_cycles = idle;
        baseline_.busy_cycles = busy;
    }

    #ifdef CONFIG_ELCO5300_STATS
    co5300_stats_reset(display_dev_);
    #endif
    #ifdef CONFIG_MSPI_ESP32_STATS
    mspi_esp32_stats_reset(mspi_dev_);
    #endif
}

    #define KV(name, value) shell_print(sh, name "=%llu", static_cast<unsigned long long>(value))

void PrintStats(const shell* sh) {
    Counters c;
    Baseline b;
    K_SPINLOCK(&lock_) {
        c = counters_;
        b = baseline_;
    }

    uint64_t idle = 0;
    uint64_t busy = 0;
    ReadAll(idle, busy);
    const uint64_t renderer = ThreadCycles(renderer_tid_.load());
    const uint64_t flush = ThreadCycles(FlushTid());

    const int64_t elapsed_ms = MAX(k_uptime_get() - b.uptime_ms, 1);

    KV("elapsed_ms", elapsed_ms);
    KV("refreshes", c.refreshes);
    KV("fps_x10", static_cast<uint64_t>(c.refreshes) * 10000 / elapsed_ms);
    KV("refr_avg_us", c.refreshes > 0 ? CyclesToUs(c.refr_cycles) / c.refreshes : 0);
    KV("refr_max_us", CyclesToUs(c.refr_cycles_max));
    KV("areas", c.areas);
    KV("wait_total_us", CyclesToUs(c.wait_cycles));
    KV("wait_max_us", CyclesToUs(c.wait_cycles_max));
    #ifdef CONFIG_SCHED_THREAD_USAGE
    KV("renderer_cpu_us", CyclesToUs(renderer - b.renderer_cycles));
        #ifdef CONFIG_LV_Z_FLUSH_THREAD
    KV("flush_cpu_us", CyclesToUs(flush - b.flush_cycles));
        #endif
    #endif
    #ifdef CONFIG_SCHED_THREAD_USAGE_ALL
    {
        const uint64_t idle_delta = idle - b.idle_cycles;
        const uint64_t total_delta = idle_delta + (busy - b.busy_cycles);
        KV("idle_pct", total_delta > 0 ? idle_delta * 100 / total_delta : 0);
    }
    #endif
    ARG_UNUSED(renderer);
    ARG_UNUSED(flush);
    ARG_UNUSED(idle);
    ARG_UNUSED(busy);

    #ifdef CONFIG_ELCO5300_STATS
    {
        co5300_stats s;
        co5300_stats_get(display_dev_, &s);

        KV("co5300.writes", s.writes);
        KV("co5300.frames", s.frames);
        KV("co5300.lock_takes", s.lock_takes);
        KV("co5300.caset", s.caset);
        KV("co5300.paset", s.paset);
        KV("co5300.ramwr", s.ramwr);
        KV("co5300.pixel_transfers", s.pixel_transfers);
        KV("co5300.pixel_bytes", s.pixel_bytes);
        KV("co5300.write_us", CyclesToUs(s.write_cycles));
        KV("co5300.write_us_max", CyclesToUs(s.write_cycles_max));
        KV("co5300.post_samples", s.post_samples);
        KV("co5300.post_lifted", s.post_lifted);
        KV("co5300.post_us", CyclesToUs(s.post_cycles));
        KV("co5300.post_us_max", CyclesToUs(s.post_cycles_max));
        KV("co5300.post_us_max_lifted", CyclesToUs(s.post_cycles_max_lifted));
        shell_print(
            sh, "co5300.post_hist=%u,%u,%u,%u,%u", s.post_hist[0], s.post_hist[1], s.post_hist[2], s.post_hist[3],
            s.post_hist[4]
        );
        KV("co5300.te_waits", s.te_waits);
        KV("co5300.te_timeouts", s.te_timeouts);
        KV("co5300.te_wait_us", CyclesToUs(s.te_wait_cycles));
        KV("co5300.write_retries", s.write_retries);
        KV("co5300.write_failures", s.write_failures);
        KV("co5300.other_cmds", s.other_cmds);

        KV("co5300.w_full_count", s.w_full_count);
        KV("co5300.w_full_us", CyclesToUs(s.w_full_cycles));
        KV("co5300.w_full_min_us", s.w_full_count > 0 ? CyclesToUs(s.w_full_min) : 0);
        KV("co5300.w_full_max_us", CyclesToUs(s.w_full_max));
        KV("co5300.w_part_count", s.w_part_count);
        KV("co5300.w_part_us", CyclesToUs(s.w_part_cycles));
        KV("co5300.w_part_min_us", s.w_part_count > 0 ? CyclesToUs(s.w_part_min) : 0);
        KV("co5300.w_part_max_us", CyclesToUs(s.w_part_max));
        KV("co5300.w_resent", s.w_resent);
        // < 22 ms, 0.5 ms bins up to 25 ms, >= 25 ms
        shell_print(
            sh, "co5300.frame_w_hist=%u,%u,%u,%u,%u,%u,%u,%u", s.w_full_hist[0], s.w_full_hist[1], s.w_full_hist[2],
            s.w_full_hist[3], s.w_full_hist[4], s.w_full_hist[5], s.w_full_hist[6], s.w_full_hist[7]
        );

        KV("co5300.te_falls", s.te_falls);
        KV("co5300.te_rises", s.te_rises);
        KV("co5300.te_coalesced", s.te_coalesced);
        KV("co5300.te_s_us100", Us100(s.te_s_cycles));
        KV("co5300.te_s_valid", s.te_s_valid ? 1 : 0);
        KV("co5300.te_iv_min_us100", s.te_iv_count > 0 ? Us100(s.te_iv_min) : 0);
        KV("co5300.te_iv_max_us100", Us100(s.te_iv_max));
        KV("co5300.te_tvdl_avg_us", s.te_tvdl_count > 0 ? CyclesToUs(s.te_tvdl_sum / s.te_tvdl_count) : 0);
        KV("co5300.te_tvdl_max_us", CyclesToUs(s.te_tvdl_max));
        KV("co5300.te_tvdh_avg_us", s.te_tvdh_count > 0 ? CyclesToUs(s.te_tvdh_sum / s.te_tvdh_count) : 0);
        KV("co5300.te_tvdh_max_us", CyclesToUs(s.te_tvdh_max));

        #ifdef CONFIG_ELCO5300_TE_WINDOW
        KV("co5300.te_win_on", s.te_win_on ? 1 : 0);
        KV("co5300.te_win_margin_us", s.te_win_margin_us);
        KV("co5300.te_win_wmax_us", s.te_win_wmax_us);
        KV("co5300.te_win_wmin_us", s.te_win_wmin_us);
        KV("co5300.te_win_starts_now", s.te_win_starts_now);
        KV("co5300.te_win_waits", s.te_win_waits);
        KV("co5300.te_win_edge_waits", s.te_win_edge_waits);
        KV("co5300.te_win_edge_timeouts", s.te_win_edge_timeouts);
        KV("co5300.te_win_fallbacks", s.te_win_fallbacks);
        KV("co5300.te_win_forced", s.te_win_forced);
        KV("co5300.te_win_wait_us", CyclesToUs(s.te_win_wait_cycles));
        KV("co5300.te_win_late", s.te_win_late);
        KV("co5300.te_win_late_resend", s.te_win_late_resend);
        // Eight equal bins over the scan period, from the falling edge
        shell_print(
            sh, "co5300.te_win_phase_hist=%u,%u,%u,%u,%u,%u,%u,%u", s.te_win_phase_hist[0], s.te_win_phase_hist[1],
            s.te_win_phase_hist[2], s.te_win_phase_hist[3], s.te_win_phase_hist[4], s.te_win_phase_hist[5],
            s.te_win_phase_hist[6], s.te_win_phase_hist[7]
        );
        // Scans a full frame stayed up: 1, 2, 3, 4, 5 or more
        shell_print(
            sh, "co5300.te_win_scans_hist=%u,%u,%u,%u,%u", s.te_win_scans_hist[0], s.te_win_scans_hist[1],
            s.te_win_scans_hist[2], s.te_win_scans_hist[3], s.te_win_scans_hist[4]
        );
        #endif
    }
    #endif

    #ifdef CONFIG_MSPI_ESP32_STATS
    {
        mspi_esp32_stats s;
        mspi_esp32_stats_get(mspi_dev_, &s);

        KV("mspi.transactions", s.transactions);
        KV("mspi.dma_transactions", s.dma_transactions);
        KV("mspi.reg_transactions", s.reg_transactions);
        KV("mspi.short_transactions", s.short_transactions);
        KV("mspi.bounce_copies", s.bounce_copies);
        KV("mspi.bounce_bytes", s.bounce_bytes);
        KV("mspi.bounce_us", CyclesToUs(s.bounce_cycles));
        KV("mspi.wait_us", CyclesToUs(s.wait_cycles));
        KV("mspi.timeouts", s.timeouts);
        KV("mspi.gaps", s.gaps);
        KV("mspi.gap_us", CyclesToUs(s.gap_cycles));
        KV("mspi.gap_us_max", CyclesToUs(s.gap_cycles_max));
        KV("mspi.psram_transactions", s.psram_transactions);
        KV("mspi.psram_underruns", s.psram_underruns);
        KV("mspi.injected_faults", s.injected_faults);
    }
    #endif
}

bool IsOn(const char* arg, bool& on) {
    if(strcmp(arg, "on") == 0) {
        on = true;
        return true;
    }
    if(strcmp(arg, "off") == 0) {
        on = false;
        return true;
    }
    return false;
}

int CmdFull(const shell* sh, size_t argc, char** argv) {
    bool on = false;
    if(argc != 2 || !IsOn(argv[1], on)) {
        shell_print(sh, "usage: ui_bench full on|off");
        return -EINVAL;
    }

    full_.store(on);
    return 0;
}

    #ifndef CONFIG_ELCO5300_STATS
int CmdTe(const shell* sh, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_error(sh, "te needs CONFIG_ELCO5300_STATS");
    return -ENOTSUP;
}

int CmdTecal(const shell* sh, size_t argc, char** argv) {
    return CmdTe(sh, argc, argv);
}
    #endif

    #ifdef CONFIG_ELCO5300_STATS
int CmdTe(const shell* sh, size_t argc, char** argv) {
    bool on = false;
    if(argc != 2 || !IsOn(argv[1], on)) {
        shell_print(sh, "usage: ui_bench te on|off");
        return -EINVAL;
    }

    const int ret = co5300_te_irq_enable(display_dev_, on);
    if(ret != 0) {
        shell_error(sh, "te %s failed: %d", on ? "on" : "off", ret);
        return ret;
    }
    return 0;
}
    #endif

    #if defined(CONFIG_ELCO5300_STATS) && defined(CONFIG_ELCO5300_TE_WINDOW)
int CmdTewin(const shell* sh, size_t argc, char** argv) {
    bool on = false;
    if(argc != 2 || !IsOn(argv[1], on)) {
        shell_print(sh, "usage: ui_bench tewin on|off");
        return -EINVAL;
    }

    const int ret = co5300_te_window_enable(display_dev_, on);
    if(ret != 0) {
        shell_error(sh, "tewin %s failed: %d", on ? "on" : "off", ret);
        return ret;
    }
    return 0;
}
    #else
int CmdTewin(const shell* sh, size_t argc, char** argv) {
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_error(sh, "tewin needs CONFIG_ELCO5300_STATS and CONFIG_ELCO5300_TE_WINDOW");
    return -ENOTSUP;
}
    #endif

// The moving bar: a full-white strip as wide as BAR_WIDTH_PX, as tall as the screen, on the top layer.
constexpr int32_t BAR_WIDTH_PX = 40;
constexpr int32_t BAR_STEP_PX = 11;

// Both only touched under the LVGL lock.
lv_obj_t* bar_ = nullptr;
lv_timer_t* bar_timer_ = nullptr;

// Runs inside lv_timer_handler(), under the LVGL lock.
void BarTimer(lv_timer_t* timer) {
    ARG_UNUSED(timer);

    if(bar_ == nullptr)
        return;

    const int32_t width = lv_display_get_horizontal_resolution(nullptr);
    lv_obj_set_x(bar_, (lv_obj_get_x(bar_) + BAR_STEP_PX) % width);
}

void BarSet(bool on) {
    ScopedLvglLock lvgl_guard;

    if(on == (bar_ != nullptr))
        return;

    if(!on) {
        lv_timer_delete(bar_timer_);
        lv_obj_delete(bar_);
        bar_timer_ = nullptr;
        bar_ = nullptr;
        return;
    }

    bar_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(bar_);
    lv_obj_set_clickable(bar_, false);
    lv_obj_set_scrollable(bar_, false);
    lv_obj_set_size(bar_, BAR_WIDTH_PX, lv_display_get_vertical_resolution(nullptr));
    lv_obj_set_pos(bar_, 0, 0);
    lv_obj_set_style_bg_color(bar_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bar_, LV_OPA_COVER, 0);
    bar_timer_ = lv_timer_create(BarTimer, LV_DEF_REFR_PERIOD, nullptr);
}

int CmdBar(const shell* sh, size_t argc, char** argv) {
    bool on = false;
    if(argc != 2 || !IsOn(argv[1], on)) {
        shell_print(sh, "usage: ui_bench bar on|off");
        return -EINVAL;
    }

    BarSet(on);
    return 0;
}

// The slider sweep: moves the first slider of the active screen like a finger would, one drag tick per timer period,
// so the writes that follow a drag (sensor, needle, bar, number, chart) run without a hand on the glass.
constexpr uint32_t SWEEP_DEFAULT_PERIOD_MS = 16; // the S3's refresh period, the rate a drag's ticks arrive at
constexpr int32_t SWEEP_DEFAULT_STEP = 1;
constexpr int32_t SWEEP_PERIOD_MIN_MS = 5;
constexpr int32_t SWEEP_PERIOD_MAX_MS = 1000;
constexpr int32_t SWEEP_STEP_MAX = 1000;

// All only touched under the LVGL lock. The position is kept here: the slider's inbound binding writes it too.
lv_timer_t* sweep_timer_ = nullptr;
int32_t sweep_step_ = SWEEP_DEFAULT_STEP;
int32_t sweep_pos_ = 0;
int32_t sweep_dir_ = 1;

lv_obj_t* FindSlider(lv_obj_t* parent) {
    const uint32_t count = lv_obj_get_child_count(parent);
    for(uint32_t i = 0; i < count; i++) {
        lv_obj_t* child = lv_obj_get_child(parent, static_cast<int32_t>(i));
        if(lv_obj_check_type(child, &lv_slider_class))
            return child;
        if(lv_obj_t* found = FindSlider(child))
            return found;
    }
    return nullptr;
}

// Looked up on every tick: a rebuilt UI must not leave a pointer behind.
lv_obj_t* FindActiveSlider() {
    lv_obj_t* screen = lv_screen_active();
    if(screen == nullptr)
        return nullptr;
    if(lv_obj_check_type(screen, &lv_slider_class))
        return screen;
    return FindSlider(screen);
}

// Runs inside lv_timer_handler(), under the LVGL lock.
void SweepTimer(lv_timer_t* timer) {
    ARG_UNUSED(timer);

    lv_obj_t* slider = FindActiveSlider();
    if(slider == nullptr)
        return;

    const int32_t low = lv_slider_get_min_value(slider);
    const int32_t high = lv_slider_get_max_value(slider);

    int32_t next = sweep_pos_ + sweep_dir_ * sweep_step_;
    if(next >= high) {
        next = high;
        sweep_dir_ = -1;
    } else if(next <= low) {
        next = low;
        sweep_dir_ = 1;
    }
    sweep_pos_ = next;

    lv_slider_set_value(slider, sweep_pos_, LV_ANIM_OFF);
    lv_obj_send_event(slider, LV_EVENT_VALUE_CHANGED, nullptr);
}

bool ParseRange(const char* text, int32_t low, int32_t high, int32_t& out) {
    char* end = nullptr;
    const long value = strtol(text, &end, 10);
    if(end == text || *end != '\0' || value < low || value > high)
        return false;
    out = static_cast<int32_t>(value);
    return true;
}

int CmdSweep(const shell* sh, size_t argc, char** argv) {
    bool on = false;
    int32_t period_ms = SWEEP_DEFAULT_PERIOD_MS;
    int32_t step = SWEEP_DEFAULT_STEP;

    const bool valid = argc >= 2
        && argc <= 4
        && IsOn(argv[1], on)
        && (on || argc == 2)
        && (argc < 3 || ParseRange(argv[2], SWEEP_PERIOD_MIN_MS, SWEEP_PERIOD_MAX_MS, period_ms))
        && (argc < 4 || ParseRange(argv[3], 1, SWEEP_STEP_MAX, step));
    if(!valid) {
        shell_print(
            sh, "usage: ui_bench sweep on [period_ms %d..%d] [step 1..%d] | off", SWEEP_PERIOD_MIN_MS,
            SWEEP_PERIOD_MAX_MS, SWEEP_STEP_MAX
        );
        return -EINVAL;
    }

    ScopedLvglLock lvgl_guard;

    if(sweep_timer_ != nullptr) {
        lv_timer_delete(sweep_timer_);
        sweep_timer_ = nullptr;
    }

    if(!on) {
        shell_print(sh, "sweep off");
        return 0;
    }

    if(lv_obj_t* slider = FindActiveSlider()) {
        sweep_pos_ = lv_slider_get_value(slider);
    } else {
        sweep_pos_ = 0;
        shell_warn(sh, "no slider on the active screen yet; the sweep starts when one appears");
    }
    sweep_dir_ = 1;
    sweep_step_ = step;
    sweep_timer_ = lv_timer_create(SweepTimer, static_cast<uint32_t>(period_ms), nullptr);
    shell_print(sh, "sweep on: period=%d ms step=%d", period_ms, step);
    return 0;
}

    #ifdef CONFIG_ELCO5300_STATS
constexpr size_t TECAL_CAPTURE_A = 64; // mode 1: about 32 scans
constexpr size_t TECAL_CAPTURE_B = 40; // scan-line mode: 20 frames, two rises skipped
constexpr size_t TECAL_SKIP_RISES = 2;
constexpr size_t TECAL_MAX_RISES = 16;
constexpr int TECAL_A_TIMEOUT_MS = 1000;
constexpr int TECAL_B_TIMEOUT_MS = 500;
constexpr int TECAL_SETTLE_MS = 100; // mode 1 takes effect on the frame after the command

const uint16_t TECAL_DEFAULT_LINES[] = { 2, 8, 9, 10, 11, 12, 13, 14, 50, 100, 200, 300, 400, 440, 465, 470 };

// Static: the shell thread's stack is 4 KB.
co5300_te_sample tecal_samples_[TECAL_CAPTURE_A];

double CyclesToUsF(double cycles) {
    return cycles * 1e6 / static_cast<double>(sys_clock_hw_cycles_per_sec());
}

// Microseconds with two decimals, without the float printf support.
struct UsText {
    char text[24];

    explicit UsText(double us) {
        const long long hundredths = std::llround(us * 100.0);
        const long long absolute = hundredths < 0 ? -hundredths : hundredths;
        snprintf(text, sizeof(text), "%s%lld.%02lld", hundredths < 0 ? "-" : "", absolute / 100, absolute % 100);
    }
};

struct ModeOne {
    double s_cycles = 0;    // mean falling-edge interval
    uint64_t last_fall = 0; // E
    double tvdl_cycles = 0;
    double tvdh_cycles = 0;
};

// Mode 1 capture of TECAL_CAPTURE_A edges. False when the unit gave too few edges.
bool CaptureModeOne(ModeOne& out) {
    size_t got = 0;
    if(co5300_te_capture_start(display_dev_, tecal_samples_, TECAL_CAPTURE_A) != 0)
        return false;

    co5300_te_capture_wait(display_dev_, K_MSEC(TECAL_A_TIMEOUT_MS), &got);

    size_t falls = 0;
    size_t tvdl_n = 0;
    size_t tvdh_n = 0;
    uint64_t first_fall = 0;
    uint64_t sum_tvdl = 0;
    uint64_t sum_tvdh = 0;
    bool have_fall = false;
    bool have_rise = false;
    uint64_t fall = 0;
    uint64_t rise = 0;
    for(size_t i = 0; i < got; i++) {
        const co5300_te_sample& e = tecal_samples_[i];
        if(e.level == 0) {
            if(have_rise) {
                sum_tvdh += e.cycles - rise;
                tvdh_n++;
            }
            if(falls == 0)
                first_fall = e.cycles;
            falls++;
            fall = e.cycles;
            have_fall = true;
        } else {
            if(have_fall) {
                sum_tvdl += e.cycles - fall;
                tvdl_n++;
            }
            rise = e.cycles;
            have_rise = true;
        }
    }

    if(falls < 3 || tvdl_n == 0 || tvdh_n == 0)
        return false;

    out.s_cycles = static_cast<double>(fall - first_fall) / static_cast<double>(falls - 1);
    out.last_fall = fall;
    out.tvdl_cycles = static_cast<double>(sum_tvdl) / static_cast<double>(tvdl_n);
    out.tvdh_cycles = static_cast<double>(sum_tvdh) / static_cast<double>(tvdh_n);
    return true;
}

// The rising edges one line produced (after the skipped ones); the phase is computed once the next mode-1 reference
// is known. Returns the number of edges kept.
uint64_t tecal_rises_[TECAL_MAX_RISES];

size_t CaptureLine(uint16_t line, const ModeOne& ref) {
    size_t got = 0;
    if(co5300_te_set_scanline(display_dev_, line) != 0)
        return 0;

    if(co5300_te_capture_start(display_dev_, tecal_samples_, TECAL_CAPTURE_B) != 0)
        return 0;

    co5300_te_capture_wait(display_dev_, K_MSEC(TECAL_B_TIMEOUT_MS), &got);

    size_t rises = 0;
    size_t used = 0;
    for(size_t i = 0; i < got && used < TECAL_MAX_RISES; i++) {
        const co5300_te_sample& e = tecal_samples_[i];
        if(e.level == 0 || e.cycles <= ref.last_fall)
            continue;

        if(++rises <= TECAL_SKIP_RISES)
            continue;

        tecal_rises_[used++] = e.cycles;
    }
    return used;
}

// The mean phase of the kept rising edges after the falling edge E of `before`, in (-S/2, S/2]. S is taken from the
// span between the falling edges of the two mode-1 references around the line: the panel's own period wanders by more
// than the error budget over the tens of scans between them, which a period measured before the line cannot follow.
double LinePhaseCycles(const ModeOne& before, const ModeOne& after, size_t used) {
    const double span = static_cast<double>(after.last_fall - before.last_fall);
    const double s_fit = span / std::round(span / before.s_cycles);

    double sum = 0;
    for(size_t i = 0; i < used; i++) {
        const double t = static_cast<double>(tecal_rises_[i] - before.last_fall);
        sum += t - std::round(t / s_fit) * s_fit;
    }
    return sum / static_cast<double>(used);
}

int CmdTecal(const shell* sh, size_t argc, char** argv) {
    uint16_t lines[16];
    size_t line_count = 0;
    if(argc > 1) {
        for(size_t i = 1; i < argc; i++) {
            char* end = nullptr;
            const long v = strtol(argv[i], &end, 10);
            if(*end != '\0' || v < 1 || v > 65535) {
                shell_print(sh, "usage: ui_bench tecal [line ...]  (lines 1..65535)");
                return -EINVAL;
            }
            lines[line_count++] = static_cast<uint16_t>(v);
        }
    } else {
        for(const uint16_t line : TECAL_DEFAULT_LINES)
            lines[line_count++] = line;
    }

    // Start from mode 1 whatever an earlier run left.
    int result = co5300_te_set_scanline(display_dev_, 0);
    if(result != 0) {
        shell_error(sh, "tecal: TE mode 1 failed: %d", result);
        return result;
    }
    k_msleep(TECAL_SETTLE_MS);

    for(size_t i = 0; i < line_count; i++) {
        ModeOne ref;
        if(!CaptureModeOne(ref)) {
            shell_error(sh, "tecal: too few TE edges in mode 1 (te off, or the screen is not scanning?)");
            result = -ETIMEDOUT;
            break;
        }

        if(i == 0) {
            shell_print(
                sh, "tecal ref S_us=%s tvdl_us=%s tvdh_us=%s", UsText(CyclesToUsF(ref.s_cycles)).text,
                UsText(CyclesToUsF(ref.tvdl_cycles)).text, UsText(CyclesToUsF(ref.tvdh_cycles)).text
            );
        }

        const size_t pulses = CaptureLine(lines[i], ref);

        // Always back to mode 1 before the next line, and on every error path.
        const int restore = co5300_te_set_scanline(display_dev_, 0);
        if(restore != 0) {
            shell_error(sh, "tecal: TE mode 1 failed: %d", restore);
            result = restore;
            break;
        }
        k_msleep(TECAL_SETTLE_MS);

        if(pulses == 0) {
            shell_print(sh, "tecal line=%u phase_us=0.00 pulses=0", lines[i]);
            continue;
        }

        ModeOne after;
        if(!CaptureModeOne(after)) {
            shell_error(sh, "tecal: too few TE edges in mode 1 after line %u", lines[i]);
            result = -ETIMEDOUT;
            break;
        }

        shell_print(
            sh, "tecal line=%u phase_us=%s pulses=%u", lines[i],
            UsText(CyclesToUsF(LinePhaseCycles(ref, after, pulses))).text, static_cast<unsigned>(pulses)
        );
    }

    co5300_te_set_scanline(display_dev_, 0);
    shell_print(sh, "tecal done");
    return result;
}
    #endif

int CmdStats(const shell* sh, size_t argc, char** argv) {
    if(argc == 1) {
        PrintStats(sh);
        return 0;
    }

    if(argc == 2 && strcmp(argv[1], "reset") == 0) {
        ResetStats();
        return 0;
    }

    shell_print(sh, "usage: ui_bench stats [reset]");
    return -EINVAL;
}

int CmdRun(const shell* sh, size_t argc, char** argv) {
    char* end = nullptr;
    const long warmup_ms = argc == 3 ? strtol(argv[1], &end, 10) : -1;
    const bool warmup_ok = end != nullptr && *end == '\0';
    const long window_ms = argc == 3 ? strtol(argv[2], &end, 10) : -1;
    if(argc != 3 || !warmup_ok || *end != '\0' || warmup_ms < 0 || window_ms <= 0) {
        shell_print(sh, "usage: ui_bench run <warmup_ms> <window_ms>");
        return -EINVAL;
    }

    full_.store(true);
    k_msleep(warmup_ms);
    ResetStats();
    k_msleep(window_ms);
    PrintStats(sh);
    full_.store(false);
    shell_print(sh, "ui_bench run done");
    return 0;
}

int CmdTrace(const shell* sh, size_t argc, char** argv) {
    #if LV_USE_PROFILER_BUILTIN
    if(argc == 1) {
        ScopedLvglLock lvgl_guard;
        lv_profiler_builtin_flush();
        return 0;
    }

    bool on = false;
    if(argc == 2 && IsOn(argv[1], on)) {
        ScopedLvglLock lvgl_guard;
        lv_profiler_builtin_set_enable(on);
        return 0;
    }

    shell_print(sh, "usage: ui_bench trace [on|off]");
    return -EINVAL;
    #else
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    shell_error(sh, "LV_USE_PROFILER_BUILTIN is off");
    return -ENOTSUP;
    #endif
}

const char* CoverName(lv_cover_res_t res) {
    switch(res) {
        case LV_COVER_RES_COVER:
            return "COVER";
        case LV_COVER_RES_NOT_COVER:
            return "NOT_COVER";
        case LV_COVER_RES_MASKED:
            return "MASKED";
        default:
            return "?";
    }
}

// Mirrors lv_refr_get_top_obj() (core/lv_refr.c), printing every object it visits. Keep the two in step.
lv_obj_t* CoverWalk(const shell* sh, const lv_area_t* area, lv_obj_t* obj, int depth) {
    const lv_obj_class_t* cls = lv_obj_get_class(obj);
    const char* name = cls != nullptr && cls->name != nullptr ? cls->name : "?";
    const int32_t child_count = static_cast<int32_t>(lv_obj_get_child_count(obj));
    const int indent = depth * 2;

    const char* reject = nullptr;
    if(!lv_area_is_in(area, &obj->coords, 0))
        reject = "area not inside coords";
    else if(lv_obj_is_hidden(obj))
        reject = "hidden";
    else if(lv_obj_get_layer_type(obj) != LV_LAYER_TYPE_NONE)
        reject = "layer type";
    else if(lv_obj_get_style_opa_internal(obj, LV_PART_MAIN) < LV_OPA_MAX)
        reject = "opa below max";

    const int layer_type = static_cast<int>(lv_obj_get_layer_type(obj));
    const int opa = static_cast<int>(lv_obj_get_style_opa_internal(obj, LV_PART_MAIN));

    if(reject != nullptr) {
        shell_print(
            sh, "%*s%s coords=%d,%d,%d,%d opa=%d layer=%d children=%d -> skipped (%s)", indent, "", name,
            static_cast<int>(obj->coords.x1), static_cast<int>(obj->coords.y1), static_cast<int>(obj->coords.x2),
            static_cast<int>(obj->coords.y2), opa, layer_type, static_cast<int>(child_count), reject
        );
        return nullptr;
    }

    lv_cover_check_info_t info;
    info.res = LV_COVER_RES_COVER;
    info.area = area;
    lv_obj_send_event(obj, LV_EVENT_COVER_CHECK, &info);

    shell_print(
        sh, "%*s%s coords=%d,%d,%d,%d opa=%d layer=%d cover=%s children=%d", indent, "", name,
        static_cast<int>(obj->coords.x1), static_cast<int>(obj->coords.y1), static_cast<int>(obj->coords.x2),
        static_cast<int>(obj->coords.y2), opa, layer_type, CoverName(info.res), static_cast<int>(child_count)
    );

    if(info.res == LV_COVER_RES_MASKED)
        return nullptr;

    lv_obj_t* found = nullptr;
    for(int32_t i = child_count - 1; i >= 0; --i) {
        found = CoverWalk(sh, area, lv_obj_get_child(obj, i), depth + 1);
        if(found != nullptr)
            break;
    }

    if(found == nullptr && info.res == LV_COVER_RES_COVER)
        found = obj;

    return found;
}

int CmdCover(const shell* sh, size_t argc, char** argv) {
    lv_display_t* display = lv_display_get_default();

    lv_area_t area;
    lv_area_set(
        &area, 0, 0, lv_display_get_horizontal_resolution(display) - 1, lv_display_get_vertical_resolution(display) - 1
    );

    if(argc == 5) {
        area.x1 = static_cast<int32_t>(strtol(argv[1], nullptr, 10));
        area.y1 = static_cast<int32_t>(strtol(argv[2], nullptr, 10));
        area.x2 = static_cast<int32_t>(strtol(argv[3], nullptr, 10));
        area.y2 = static_cast<int32_t>(strtol(argv[4], nullptr, 10));
    } else if(argc != 1) {
        shell_print(sh, "usage: ui_bench cover [x1 y1 x2 y2]");
        return -EINVAL;
    }

    ScopedLvglLock lvgl_guard;

    shell_print(sh, "area=%d,%d,%d,%d", (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2);

    lv_obj_t* top = CoverWalk(sh, &area, lv_screen_active(), 0);
    if(top != nullptr) {
        const lv_obj_class_t* cls = lv_obj_get_class(top);
        shell_print(
            sh, "draws from: %s coords=%d,%d,%d,%d", cls != nullptr ? cls->name : "?", (int)top->coords.x1,
            (int)top->coords.y1, (int)top->coords.x2, (int)top->coords.y2
        );
    } else {
        shell_print(sh, "draws from: none (bottom layer, then the active screen)");
    }

    shell_print(sh, "layer_top children=%d", (int)lv_obj_get_child_count(lv_display_get_layer_top(display)));
    shell_print(sh, "layer_bottom children=%d", (int)lv_obj_get_child_count(lv_display_get_layer_bottom(display)));
    return 0;
}

int CmdHeap(const shell* sh, size_t argc, char** argv) {
    ARG_UNUSED(sh);
    ARG_UNUSED(argc);
    ARG_UNUSED(argv);
    SystemInfo::PrintHeapInfo();
    return 0;
}

// NOLINTBEGIN(modernize-use-nullptr): Zephyr's shell macros mix these arguments with NULL in ?:
SHELL_STATIC_SUBCMD_SET_CREATE(
    sub_ui_bench,
    SHELL_CMD_ARG(full, NULL, "full on|off: invalidate the whole screen every refresh period", CmdFull, 2, 0),
    SHELL_CMD_ARG(te, NULL, "te on|off: the TE interrupt", CmdTe, 2, 0),
    SHELL_CMD_ARG(tewin, NULL, "tewin on|off: the window rule (off: the edge rule)", CmdTewin, 2, 0),
    SHELL_CMD_ARG(
        tecal,
        NULL,
        "tecal [line ...]: time from TE's falling edge to scan line N, per line",
        CmdTecal,
        1,
        16
    ),
    SHELL_CMD_ARG(
        bar,
        NULL,
        "bar on|off: a white bar moving over the screen, content changes every refresh",
        CmdBar,
        2,
        0
    ),
    SHELL_CMD_ARG(
        sweep,
        NULL,
        "sweep on [period_ms] [step] | off: move the first slider back and forth, one drag tick per period",
        CmdSweep,
        2,
        2
    ),
    SHELL_CMD_ARG(stats, NULL, "stats [reset]: counters since the last reset", CmdStats, 1, 1),
    SHELL_CMD_ARG(run, NULL, "run <warmup_ms> <window_ms>: full mode, warm up, reset, measure, print", CmdRun, 3, 0),
    SHELL_CMD_ARG(trace, NULL, "trace [on|off]: LVGL profiler switch, or dump", CmdTrace, 1, 1),
    SHELL_CMD_ARG(cover, NULL, "cover [x1 y1 x2 y2]: the top-object search LVGL runs for an area", CmdCover, 1, 4),
    SHELL_CMD_ARG(heap, NULL, "heap: heap statistics", CmdHeap, 1, 0),
    SHELL_SUBCMD_SET_END
);

} // namespace

SHELL_CMD_REGISTER(ui_bench, &sub_ui_bench, "Display bench probes", NULL);
// NOLINTEND(modernize-use-nullptr)

void UiBench::Install(lv_display_t* display, int renderer_priority) {
    renderer_priority_ = renderer_priority;

    ScopedLvglLock lvgl_guard;

    lv_display_add_event_cb(display, DisplayEvent, LV_EVENT_ALL, nullptr);
    lv_timer_create(FullTimer, LV_DEF_REFR_PERIOD, nullptr);

    #if LV_USE_PROFILER_BUILTIN
    // lv_init() started the profiler with defaults; restart it with microsecond ticks and a printing flush, off until
    // `ui_bench trace on`. The buffer size comes from CONFIG_LV_PROFILER_BUILTIN_BUF_SIZE.
    lv_profiler_builtin_config_t config;
    lv_profiler_builtin_config_init(&config);
    config.tick_per_sec = 1000000;
    config.tick_get_cb = ProfilerTick;
    config.flush_cb = ProfilerFlush;
    lv_profiler_builtin_init(&config);
    lv_profiler_builtin_set_enable(false);
    #endif

    ResetStats();
}

} // namespace eerie_leap::utilities::dev_tools

#endif // CONFIG_EERIE_LEAP_UI_BENCH
