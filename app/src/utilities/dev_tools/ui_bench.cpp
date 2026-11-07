#ifdef CONFIG_EERIE_LEAP_UI_BENCH

    #include <atomic>
    #include <cstdint>
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
        KV("co5300.other_cmds", s.other_cmds);
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
    SHELL_CMD_ARG(stats, NULL, "stats [reset]: counters since the last reset", CmdStats, 1, 1),
    SHELL_CMD_ARG(run, NULL, "run <warmup_ms> <window_ms>: full mode, warm up, reset, measure, print", CmdRun, 3, 0),
    SHELL_CMD_ARG(trace, NULL, "trace [on|off]: LVGL profiler switch, or dump", CmdTrace, 1, 1),
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
