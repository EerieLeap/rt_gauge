#include <algorithm>
#include <bit>
#include <cerrno>
#include <cstring>
#include <exception>
#include <utility>

#include <lvgl_private.h>
#include <zephyr/logging/log.h>

#include "utilities/memory/memory_resource_manager.h"

#include "frame_batcher.h"

namespace eerie_leap::domain::ui_domain::services::frame_flush {

using eerie_leap::utilities::memory::Mrm;

LOG_MODULE_REGISTER(frame_batcher_logger);

namespace {

// The cache line of the S3 and the P4: every rect starts on one, as it did before packing.
constexpr size_t RECT_ALIGN = 64;

// A deferred repair needs this many failing frames in a row; before it, each failed rect is invalidated on its own.
constexpr uint32_t DEFER_AFTER_FRAMES = 3;
constexpr uint32_t REPAIR_PERIOD_MS = 1000;

constexpr size_t RoundUp(size_t value, size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

constexpr size_t RoundDown(size_t value, size_t alignment) {
    return value / alignment * alignment;
}

constexpr uint8_t Bit(int array) {
    return static_cast<uint8_t>(1U << array);
}

// What LVGL's reshape reserves for an area of this size.
size_t AreaBytes(int32_t w, int32_t h) {
    return RoundUp(static_cast<size_t>(w) * 2U * static_cast<size_t>(h), LV_DRAW_BUF_ALIGN);
}

} // namespace

FrameBatcher* FrameBatcher::instance = nullptr;

FrameBatcher::~FrameBatcher() {
    Uninstall();
}

int FrameBatcher::Install(lv_display_t* display, IFrameSink& sink, void (*wake)(void*), void* wake_ctx) {
    const auto refuse = [this](int error) {
        stats_.install_failed++;
        return error;
    };

    if(display == nullptr)
        return refuse(-EINVAL);
    if(instance != nullptr)
        return refuse(-EBUSY);
    if(lv_display_get_render_mode(display) != LV_DISPLAY_RENDER_MODE_PARTIAL)
        return refuse(-ENOTSUP);
    if(lv_display_get_rotation(display) != LV_DISPLAY_ROTATION_0)
        return refuse(-ENOTSUP);

    const lv_color_format_t cf = lv_display_get_color_format(display);
    if(lv_color_format_get_size(cf) != 2)
        return refuse(-ENOTSUP);
    // The rects are contiguous (pitch equal to width) only while LVGL's stride is w * 2, and a size rounded down to
    // LV_DRAW_BUF_ALIGN is consistent with the 64-byte slots only for an alignment that divides 64.
    if(LV_DRAW_BUF_STRIDE_ALIGN != 1 || (RECT_ALIGN % LV_DRAW_BUF_ALIGN) != 0)
        return refuse(-ENOTSUP);

    const int32_t hor = lv_display_get_horizontal_resolution(display);
    const int32_t ver = lv_display_get_vertical_resolution(display);
    if(hor < 1
        || ver < 1
        || lv_draw_buf_width_to_stride(static_cast<uint32_t>(hor), cf) != static_cast<uint32_t>(hor) * 2U)
        return refuse(-ENOTSUP);

    const lv_draw_buf_t* const buf[2] = { display->buf_1, display->buf_2 };
    const size_t screen_bytes = static_cast<size_t>(hor) * static_cast<size_t>(ver) * 2U;
    const size_t min_slot = RoundUp(16U * static_cast<size_t>(hor) * 2U, RECT_ALIGN);
    size_t sizes[2]{};
    for(int a = 0; a < 2; ++a) {
        if(buf[a] == nullptr || buf[a]->data == nullptr)
            return refuse(-EINVAL);
        if((std::bit_cast<uintptr_t>(buf[a]->data) % RECT_ALIGN) != 0)
            return refuse(-EINVAL);
        sizes[a] = RoundDown(buf[a]->data_size, LV_DRAW_BUF_ALIGN);
        if(sizes[a] < screen_bytes || sizes[a] < min_slot)
            return refuse(-EINVAL);
    }
    if(buf[0]->data == buf[1]->data)
        return refuse(-EINVAL);

    eerie_memory::PmrUniquePtr<Frame> tables[2];
    try {
        tables[0] = eerie_memory::make_unique_pmr<Frame>(Mrm::GetExtPmr());
        tables[1] = eerie_memory::make_unique_pmr<Frame>(Mrm::GetExtPmr());
    } catch(const std::exception&) {
        return refuse(-ENOMEM);
    }

    if(lv_draw_buf_init(&slot_, hor, ver, cf, LV_STRIDE_AUTO, buf[0]->data, sizes[0]) != LV_RESULT_OK)
        return refuse(-EINVAL);
    slot_.handlers = buf[0]->handlers;

    // Everything checked: from here on the display changes.
    display_ = display;
    sink_ = &sink;
    wake_ = wake;
    wake_ctx_ = wake_ctx;
    hor_ = hor;
    ver_ = ver;
    min_slot_ = min_slot;
    saved_buf_1_ = display->buf_1;
    saved_buf_2_ = display->buf_2;
    saved_flush_cb_ = lv_display_get_flush_cb(display);
    for(int a = 0; a < 2; ++a) {
        array_[a] = buf[a]->data;
        array_size_[a] = sizes[a];
        k_sem_init(&free_[a], 1, 1);
        frames_[a] = std::move(tables[a]);
    }
    {
        // The counters belong to this installation; a refusal before it stays counted.
        const uint32_t refused = stats_.install_failed;
        stats_ = Stats{};
        stats_.install_failed = refused;
    }
    owned_ = 0;
    cur_ = 0;
    table_ = 0;
    areas_done_ = 0;
    frame_bytes_ = 0;
    serial_ = false;
    logged_trip_ = false;
    look_ahead_ = true;
    {
        K_SPINLOCK(&lock_) {
            repair_count_ = 0;
            full_request_ = false;
            deferred_ = false;
            full_applied_ = false;
            failing_frames_ = 0;
        }
    }

    // LVGL runs single-buffered from here: no swap, and no wait before a flush because flush_ready() is always given
    // before the callback returns.
    lv_display_set_draw_buffers(display, &slot_, nullptr);
    lv_display_set_flush_cb(display, FlushCb);
    lv_display_add_event_cb(display, RenderStartCb, LV_EVENT_RENDER_START, this);

    instance = this;
    return 0;
}

void FrameBatcher::Uninstall() {
    if(instance != this)
        return;

    lv_display_remove_event_cb_with_user_data(display_, RenderStartCb, this);
    lv_display_set_flush_cb(display_, saved_flush_cb_);
    lv_display_set_draw_buffers(display_, saved_buf_1_, saved_buf_2_);

    if(repair_timer_ != nullptr) {
        lv_timer_delete(repair_timer_);
        repair_timer_ = nullptr;
    }

    frames_[0].reset();
    frames_[1].reset();
    instance = nullptr;
    display_ = nullptr;
    sink_ = nullptr;
    wake_ = nullptr;
    wake_ctx_ = nullptr;
}

void FrameBatcher::FlushCb(lv_display_t* display, const lv_area_t* area, uint8_t* px_map) {
    FrameBatcher* self = instance;
    if(self == nullptr || self->display_ != display) {
        lv_display_flush_ready(display);
        return;
    }
    self->OnFlush(area, px_map);
}

void FrameBatcher::RenderStartCb(lv_event_t* event) {
    static_cast<FrameBatcher*>(lv_event_get_user_data(event))->OnRenderStart();
}

void FrameBatcher::OnRenderStart() {
    areas_done_ = 0;
    if(serial_ || (owned_ & Bit(cur_)) != 0)
        return;

    // The array the previous frame did not use. Its batch may still be on the way to the panel; the wait counts as
    // flush for LVGL's performance monitor, as LVGL's own wait does.
    TakeArray(cur_, true);
    // The table of the frame before the last one: free, since the array taken was last used by a finished batch.
    frames_[table_]->n = 0;
    frames_[table_]->arrays = 0;
}

void FrameBatcher::TakeArray(int array, bool bracket) {
    if((owned_ & Bit(array)) != 0)
        return;

    if(k_sem_take(&free_[array], K_NO_WAIT) != 0) {
        const uint32_t start = k_cycle_get_32();
        // Inside the flush callback LVGL's monitor already counts the time as flush; a second bracket would count it twice.
        if(bracket)
            lv_display_send_event(display_, LV_EVENT_FLUSH_WAIT_START, nullptr);
        k_sem_take(&free_[array], K_FOREVER);
        if(bracket)
            lv_display_send_event(display_, LV_EVENT_FLUSH_WAIT_FINISH, nullptr);
        const uint32_t waited = k_cyc_to_us_floor32(k_cycle_get_32() - start);
        stats_.array_waits++;
        stats_.array_wait_us += waited;
        stats_.array_wait_max_us = std::max(stats_.array_wait_max_us, waited);
    }
    owned_ |= Bit(array);
}

void FrameBatcher::SetSlot(int array, const uint8_t* start) {
    uint8_t* data = array_[array] + (start - array_[array]);
    slot_.data = data;
    slot_.unaligned_data = data;
    // LVGL sizes an area's parts from this, and its reshape rounds each part up to LV_DRAW_BUF_ALIGN: rounding down
    // here keeps the two in step for any alignment that divides 64.
    slot_.data_size = static_cast<uint32_t>(RoundDown(static_cast<size_t>(ArrayEnd(array) - data), LV_DRAW_BUF_ALIGN));
}

size_t FrameBatcher::NextAreaBytes() const {
    uint32_t seen = 0;
    for(uint32_t i = 0; i < display_->inv_p; ++i) {
        if(display_->inv_area_joined[i] != 0)
            continue;
        if(seen == areas_done_) {
            const lv_area_t& area = display_->inv_areas[i];
            return AreaBytes(lv_area_get_width(&area), lv_area_get_height(&area));
        }
        ++seen;
    }
    return 0;
}

void FrameBatcher::OnFlush(const lv_area_t* area, const uint8_t* px_map) {
    const int32_t w = lv_area_get_width(area);
    const int32_t h = lv_area_get_height(area);
    const size_t bytes = AreaBytes(w, h);
    stats_.areas++;

    // Serial mode has no slots to guard: it posts whatever LVGL rendered, so a display that LVGL draws elsewhere keeps
    // working.
    if(serial_) {
        SerialFlush(area, px_map);
        return;
    }

    // LVGL rendered somewhere else, or a reshape failed and it drew with the previous part's header (silent on the
    // S3: the assert handler is empty and the log is compiled out).
    if(px_map != slot_.data
        || std::cmp_not_equal(slot_.header.w, w)
        || std::cmp_not_equal(slot_.header.h, h)
        || std::cmp_not_equal(slot_.header.stride, w * 2)) {
        TripGuard();
        lv_display_flush_ready(display_);
        return;
    }

    Frame& frame = *frames_[table_];
    frame.rects[frame.n++] = FrameRect{ px_map, static_cast<uint16_t>(area->x1), static_cast<uint16_t>(area->y1),
        static_cast<uint16_t>(w), static_cast<uint16_t>(h) };
    frame.arrays |= Bit(cur_);
    frame_bytes_ += bytes;

    // Read inside the callback: LVGL clears the flag at its next wait.
    if(lv_display_flush_is_last(display_)) {
        PostLast();
        lv_display_flush_ready(display_);
        return;
    }

    const bool area_done = display_->last_part != 0;
    if(area_done)
        areas_done_++;

    if(frame.n >= kMaxRects) {
        Break(cur_ ^ 1);
        lv_display_flush_ready(display_);
        return;
    }

    // LVGL sizes the parts of an area once, from the room it finds when the area starts, so the next part needs as
    // much as this one; the next area needs its whole size, or it would be cut into parts of the remainder's height.
    size_t need = bytes;
    bool from_look_ahead = false;
    if(area_done) {
        need = min_slot_;
        if(look_ahead_) {
            need = std::max(need, NextAreaBytes());
            from_look_ahead = true;
        }
    }
    AdvanceSlot(px_map, bytes, need, from_look_ahead);
    lv_display_flush_ready(display_);
}

void FrameBatcher::AdvanceSlot(const uint8_t* px_map, size_t bytes, size_t need, bool from_look_ahead) {
    const size_t offset = RoundUp(static_cast<size_t>(px_map - array_[cur_]) + bytes, RECT_ALIGN);
    if(offset + need <= array_size_[cur_]) {
        SetSlot(cur_, array_[cur_] + offset);
        return;
    }

    stats_.overflows++;
    if(from_look_ahead)
        stats_.lookahead_switches++;

    const int other = cur_ ^ 1;
    if((frames_[table_]->arrays & Bit(other)) != 0) {
        // Both arrays hold rects of this frame already.
        Break(other);
        return;
    }
    TakeArray(other);
    cur_ = other;
    SetSlot(cur_, array_[cur_]);
}

void FrameBatcher::PostTable() {
    Frame& frame = *frames_[table_];
    frame.sequence = ++sequence_;
    stats_.frames++;
    stats_.rects_max = std::max<uint32_t>(stats_.rects_max, frame.n);
    stats_.bytes_max = std::max<uint32_t>(stats_.bytes_max, static_cast<uint32_t>(frame_bytes_));
    frame_bytes_ = 0;

    // An array taken and not used has no batch to give it back.
    for(int a = 0; a < 2; ++a) {
        if((owned_ & Bit(a)) != 0 && (frame.arrays & Bit(a)) == 0)
            k_sem_give(&free_[a]);
    }
    owned_ = 0;

    // K_FOREVER on purpose: a slip in the protocol then stalls the renderer until the writer dequeues, where
    // dropping the frame would lose arrays nobody returns.
    sink_->Post(frame);
    table_ ^= 1;
}

void FrameBatcher::PostLast() {
    PostTable();
    // The next frame renders into the array this one did not end in, and takes it at its RENDER_START.
    cur_ ^= 1;
    SetSlot(cur_, array_[cur_]);
}

void FrameBatcher::Break(int next_array) {
    stats_.frame_breaks++;
    PostTable();
    // Every earlier batch finished once both arrays are back, so both tables are free too.
    TakeArray(0);
    TakeArray(1);
    frames_[table_]->n = 0;
    frames_[table_]->arrays = 0;
    cur_ = next_array;
    SetSlot(cur_, array_[cur_]);
}

void FrameBatcher::TripGuard() {
    stats_.guard_trips++;
    if(!logged_trip_) {
        logged_trip_ = true;
        LOG_ERR("LVGL did not render into the slot handed out: frame consistency is off");
    }
    // The area is not sent. The screen is repaired on the next refresh, which this one cannot start.
    QueueFullRepair();

    if(!serial_) {
        serial_ = true;
        if(frames_[table_]->n > 0)
            Break(cur_);
    }
    SetSlot(cur_, array_[cur_]);
}

void FrameBatcher::SerialFlush(const lv_area_t* area, const uint8_t* px_map) {
    // The stock single-buffer semantics: one rect per batch, and the next area waits for it. The batch is the only
    // one in flight, so its table is free again by the time the array comes back.
    Frame& frame = *frames_[table_];
    frame.n = 0;
    frame.arrays = Bit(cur_);
    frame.rects[frame.n++] = FrameRect{ px_map, static_cast<uint16_t>(area->x1), static_cast<uint16_t>(area->y1),
        static_cast<uint16_t>(lv_area_get_width(area)), static_cast<uint16_t>(lv_area_get_height(area)) };
    frame.sequence = ++sequence_;
    stats_.frames++;
    stats_.rects_max = std::max<uint32_t>(stats_.rects_max, 1);

    sink_->Post(frame);
    k_sem_take(&free_[cur_], K_FOREVER);
    SetSlot(cur_, array_[cur_]);
    lv_display_flush_ready(display_);
}

void FrameBatcher::QueueFullRepair() {
    K_SPINLOCK(&lock_) {
        full_request_ = true;
    }
    // From the renderer itself this leaves the semaphore given, so the renderer runs again before it sleeps.
    if(wake_ != nullptr)
        wake_(wake_ctx_);
}

void FrameBatcher::Complete(const Frame& frame, std::span<const uint8_t> failed) {
    // Everything is read before the first give: after it the renderer may reuse the frame.
    const uint8_t arrays = frame.arrays;
    const uint8_t n = frame.n;
    bool wake = false;

    K_SPINLOCK(&lock_) {
        if(failed.empty()) {
            failing_frames_ = 0;
            // A frame rendered before the full-screen repair says nothing about the screen the repair is for.
            if(deferred_ && full_applied_ && frame.sequence > full_applied_sequence_) {
                deferred_ = false;
                full_applied_ = false;
            }
        } else {
            failing_frames_++;
            // A failure after the full-screen repair went out means that repair did not land either.
            if(deferred_ && frame.sequence > full_applied_sequence_)
                full_applied_ = false;
            if(!deferred_ && failing_frames_ >= DEFER_AFTER_FRAMES) {
                deferred_ = true;
                full_request_ = true;
                full_applied_ = false;
                stats_.repairs_deferred += static_cast<uint32_t>(repair_count_);
                repair_count_ = 0;
                wake = true;
            }
            if(deferred_) {
                stats_.repairs_deferred += static_cast<uint32_t>(failed.size());
            } else {
                for(const uint8_t index : failed) {
                    if(index >= n)
                        continue;
                    const FrameRect& rect = frame.rects[index];
                    if(repair_count_ >= repairs_.size()) {
                        full_request_ = true;
                        break;
                    }
                    repairs_[repair_count_++] = Repair{ static_cast<int16_t>(rect.x), static_cast<int16_t>(rect.y),
                        static_cast<int16_t>(rect.x + rect.w - 1), static_cast<int16_t>(rect.y + rect.h - 1) };
                }
                wake = true;
            }
        }
    }

    if(wake && wake_ != nullptr)
        wake_(wake_ctx_);

    for(int a = 0; a < 2; ++a) {
        if((arrays & Bit(a)) != 0)
            k_sem_give(&free_[a]);
    }
}

void FrameBatcher::InvalidateAll() {
    lv_area_t whole;
    lv_area_set(&whole, 0, 0, hor_ - 1, ver_ - 1);
    lv_inv_area(display_, &whole);
}

void FrameBatcher::ApplyRepairs() {
    if(instance != this)
        return;

    std::array<Repair, kMaxRects> local{};
    size_t count = 0;
    bool full = false;
    bool deferred = false;

    K_SPINLOCK(&lock_) {
        count = repair_count_;
        std::copy_n(repairs_.begin(), count, local.begin());
        repair_count_ = 0;
        full = full_request_;
        full_request_ = false;
        deferred = deferred_;
        if(full) {
            full_applied_ = deferred_;
            full_applied_sequence_ = sequence_;
        }
    }

    // lv_inv_area() resumes the refresh timer that LVGL pauses after a refresh, so an idle screen is redrawn too.
    for(size_t i = 0; i < count; ++i) {
        lv_area_t area;
        lv_area_set(&area, local[i].x1, local[i].y1, local[i].x2, local[i].y2);
        lv_inv_area(display_, &area);
        stats_.repairs++;
    }

    if(full) {
        InvalidateAll();
        if(deferred && repair_timer_ == nullptr)
            repair_timer_ = lv_timer_create(RepairTimerCb, REPAIR_PERIOD_MS, this);
    }
}

void FrameBatcher::RepairTimerCb(lv_timer_t* timer) {
    auto* self = static_cast<FrameBatcher*>(lv_timer_get_user_data(timer));
    bool pending = false;
    K_SPINLOCK(&self->lock_) {
        pending = self->deferred_;
        if(pending) {
            self->full_applied_ = true;
            self->full_applied_sequence_ = self->sequence_;
        }
    }
    if(!pending) {
        self->repair_timer_ = nullptr;
        lv_timer_delete(timer);
        return;
    }
    self->InvalidateAll();
}

bool FrameBatcher::IsRepairDeferred() const {
    bool deferred = false;
    K_SPINLOCK(&lock_) {
        deferred = deferred_;
    }
    return deferred;
}

FrameBatcher::Stats FrameBatcher::GetStats() const {
    Stats copy;
    K_SPINLOCK(&lock_) {
        copy = stats_;
    }
    return copy;
}

void FrameBatcher::ResetMaxima() {
    // The renderer raises these without the lock; a race loses at most one sample.
    K_SPINLOCK(&lock_) {
        stats_.rects_max = 0;
        stats_.array_wait_max_us = 0;
        stats_.bytes_max = 0;
    }
}

} // namespace eerie_leap::domain::ui_domain::services::frame_flush
