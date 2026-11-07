#pragma once

#ifdef CONFIG_EERIE_LEAP_UI_BENCH

    #include <lvgl.h>

namespace eerie_leap::utilities::dev_tools {

// Bench-only display probes and the `ui_bench` shell command (counters since the last reset, full-screen invalidation,
// LVGL's built-in profiler). Never part of a shipped build.
class UiBench {
public:
    UiBench() = delete;

    // Call once, after LVGL and the display are up and before the renderer thread starts (or under the LVGL lock).
    // renderer_priority is the renderer thread's priority; a flush posted from a higher one counts as lifted.
    static void Install(lv_display_t* display, int renderer_priority);
};

} // namespace eerie_leap::utilities::dev_tools

#endif // CONFIG_EERIE_LEAP_UI_BENCH
