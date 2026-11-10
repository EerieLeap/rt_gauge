#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include <lvgl.h>
#include <zephyr/kernel.h>

#include "eerie_memory.hpp"

namespace eerie_leap::domain::ui_domain::services::frame_flush {

// The name is the interface of the writer glue and of the plan (section 7.3).
// NOLINTNEXTLINE(readability-identifier-naming)
inline constexpr size_t kMaxRects = 32; // LV_INV_BUF_SIZE

struct FrameRect {
    const uint8_t* buf; // w * h pixels, rows back to back, 64-byte aligned
    uint16_t x, y, w, h;
};

struct Frame {
    uint32_t sequence = 0;
    uint8_t n = 0;
    uint8_t arrays = 0; // bit a set: rects in slot array a
    std::array<FrameRect, kMaxRects> rects{};
};

class IFrameSink {
public:
    virtual ~IFrameSink() = default;
    // Renderer thread, LVGL lock held. Blocks only while the queue is full, which the batcher's protocol never lets
    // happen. The frame's pixels stay unchanged until FrameBatcher::Complete() is called for it (on any thread).
    virtual void Post(Frame& frame) = 0;
};

// Packs every area of an LVGL frame into its own slot of one of two full-size draw buffers and hands the frame to a
// sink in one piece, so the writer can put all of it on the panel as one batch. It relies on how LVGL's PARTIAL mode
// uses its draw buffer and its list of areas (plan section 4.5); the views test pins each of those behaviours.
class FrameBatcher {
public:
    struct Stats {
        uint32_t frames = 0;      // posted frames
        uint32_t areas = 0;       // flush callbacks
        uint32_t rects_max = 0;   // the largest frame
        uint32_t array_waits = 0; // the renderer waited for a slot array
        uint64_t array_wait_us = 0;
        uint32_t array_wait_max_us = 0;
        uint32_t lookahead_switches = 0; // an array switch before an area that did not fit
        uint32_t overflows = 0;          // a frame left its first array
        uint32_t frame_breaks = 0;       // a frame posted in pieces
        uint32_t guard_trips = 0;
        uint32_t repairs = 0;          // rects invalidated after failing
        uint32_t repairs_deferred = 0; // repairs folded into the deferred full-screen one
        uint32_t install_failed = 0;   // Install() refused
        uint32_t bytes_max = 0;        // the most bytes one frame used in its slot arrays
    };

    FrameBatcher() = default;
    ~FrameBatcher();
    FrameBatcher(const FrameBatcher&) = delete;
    FrameBatcher& operator=(const FrameBatcher&) = delete;
    FrameBatcher(FrameBatcher&&) = delete;
    FrameBatcher& operator=(FrameBatcher&&) = delete;

    // Takes the display's two draw buffers, its flush callback and a RENDER_START handler. Checks everything first
    // and changes nothing when a check fails: it returns a negative errno and the caller keeps the flush path it had.
    // Renderer thread or before the renderer runs, with the LVGL lock held. `wake` is called from Complete() when a
    // failure needs ApplyRepairs().
    int Install(lv_display_t* display, IFrameSink& sink, void (*wake)(void*), void* wake_ctx);

    // Gives the display its flush callback and draw buffers back. No batch may be in flight.
    void Uninstall();

    // Writer thread: the frame is on the panel, except the rects listed in `failed` (indices into frame.rects: every
    // attempt failed, or the rect was not sent). Gives the frame's slot arrays back to the renderer.
    void Complete(const Frame& frame, std::span<const uint8_t> failed);

    // Renderer thread, LVGL lock held, before lv_timer_handler(): invalidates what Complete() reported.
    void ApplyRepairs();

    Stats GetStats() const;

    // Zeroes the three maxima, which only ever grow, so a bench reset starts them over.
    void ResetMaxima();

    // True from the third failing frame in a row until a frame that was rendered after the full-screen repair
    // completes without a failure.
    bool IsRepairDeferred() const;

    // The 1 s timer that repeats the deferred full-screen repair; null while none runs.
    lv_timer_t* GetRepairTimer() const {
        return repair_timer_;
    }

    // The views test turns the look-ahead off to force an area into parts.
    void SetLookAheadForTest(bool enabled) {
        look_ahead_ = enabled;
    }

    static FrameBatcher* Instance() {
        return instance;
    }

private:
    struct Repair {
        int16_t x1, y1, x2, y2;
    };

    static FrameBatcher* instance;

    static void FlushCb(lv_display_t* display, const lv_area_t* area, uint8_t* px_map);
    static void RenderStartCb(lv_event_t* event);
    static void RepairTimerCb(lv_timer_t* timer);

    void OnFlush(const lv_area_t* area, const uint8_t* px_map);
    void OnRenderStart();

    void SerialFlush(const lv_area_t* area, const uint8_t* px_map);
    void TripGuard();
    // Advances the slot behind the rect just appended; `need` is what the next part or area wants of the array.
    void AdvanceSlot(const uint8_t* px_map, size_t bytes, size_t need, bool from_look_ahead);
    // The bytes of the next area LVGL renders after the ones this frame has finished, 0 when there is none.
    size_t NextAreaBytes() const;
    // The frame's last rect has been appended: post it and point LVGL at the other array.
    void PostLast();
    // Posts what the frame has so far, waits for every batch, and goes on from the start of `next_array`.
    void Break(int next_array);
    void PostTable();
    void TakeArray(int array, bool bracket = false);
    void SetSlot(int array, const uint8_t* start);
    void QueueFullRepair();
    void InvalidateAll();

    uint8_t* ArrayEnd(int array) const {
        return array_[array] + array_size_[array];
    }

    lv_display_t* display_ = nullptr;
    IFrameSink* sink_ = nullptr;
    void (*wake_)(void*) = nullptr;
    void* wake_ctx_ = nullptr;

    // What Uninstall() gives back.
    lv_display_flush_cb_t saved_flush_cb_ = nullptr;
    lv_draw_buf_t* saved_buf_1_ = nullptr;
    lv_draw_buf_t* saved_buf_2_ = nullptr;

    // The draw buffer LVGL renders into: one slot of one of the arrays at a time.
    lv_draw_buf_t slot_{};
    uint8_t* array_[2]{};
    size_t array_size_[2]{};
    size_t min_slot_ = 0;
    int32_t hor_ = 0;
    int32_t ver_ = 0;

    // Count 1 while the array is free for the renderer. The renderer takes one before it renders into the array, the
    // writer gives it back once the batch that used the array is on the panel.
    k_sem free_[2]{};
    uint8_t owned_ = 0; // bit a set: the renderer holds free_[a]
    int cur_ = 0;       // the array the slot is in
    int table_ = 0;     // the frame table being filled; alternates per posted batch
    eerie_memory::PmrUniquePtr<Frame> frames_[2];
    uint32_t sequence_ = 0;
    uint32_t areas_done_ = 0;
    size_t frame_bytes_ = 0;
    bool look_ahead_ = true;
    bool serial_ = false;
    bool logged_trip_ = false;

    lv_timer_t* repair_timer_ = nullptr;

    mutable k_spinlock lock_{};
    // Under lock_, between Complete() on the writer and ApplyRepairs() on the renderer.
    std::array<Repair, kMaxRects> repairs_{};
    size_t repair_count_ = 0;
    bool full_request_ = false; // a full-screen repair waits for ApplyRepairs()
    bool deferred_ = false;
    bool full_applied_ = false;
    uint32_t full_applied_sequence_ = 0;
    uint32_t failing_frames_ = 0;

    Stats stats_{};
};

} // namespace eerie_leap::domain::ui_domain::services::frame_flush
