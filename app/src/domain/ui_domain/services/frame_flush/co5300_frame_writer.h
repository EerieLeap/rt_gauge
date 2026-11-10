#pragma once

// The S3 side of the frame flush: the writer thread that puts a frame on the CO5300 in one co5300_write_frame() call.
// Everything is under CONFIG_EERIE_LEAP_UI_FRAME_FLUSH; the file is globbed into builds and test suites without it,
// where it compiles to nothing.
#ifdef CONFIG_EERIE_LEAP_UI_FRAME_FLUSH

    #include <zephyr/kernel.h>

    #include "frame_batcher.h"

namespace eerie_leap::domain::ui_domain::services::frame_flush {

// Allocates the writer's frame once. Call before FrameBatcher::Install(); a failure means the frame path cannot run.
int Co5300WriterInitialize();

// The sink to install the batcher with. Post() puts the frame on the writer's queue.
IFrameSink& Co5300FrameSink();

// The writer thread, for the bench's CPU accounting.
k_tid_t WriterTid();

// Bench diagnostic (ui_bench batch on|off). Off writes each rect through display_write() in LVGL's order, which is
// neither today's interleaved render and write nor the batch rule.
void SetBatching(bool enabled);
bool IsBatching();

    #ifdef CONFIG_ELCO5300_STATS
// Lets the bench post stamp tell a post made by a lifted renderer from a normal one.
void SetRendererPriority(int priority);
    #endif

} // namespace eerie_leap::domain::ui_domain::services::frame_flush

#endif // CONFIG_EERIE_LEAP_UI_FRAME_FLUSH
