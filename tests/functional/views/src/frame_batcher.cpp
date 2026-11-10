// The frame batcher (services/frame_flush/frame_batcher.h) on a 96x96 display in LVGL's PARTIAL mode, the way
// Zephyr's glue sets it up: two arrays of one screen each, the batcher replaces LVGL's draw buffers and flush
// callback. A fake sink plays the writer. Each test pins the LVGL behaviour the batcher relies on (plan section 4.5,
// row numbers in the comments), so an LVGL update that breaks one fails here and not on the unit.
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iterator>
#include <span>
#include <vector>

#include <lvgl.h>
#include <lvgl_private.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "domain/ui_domain/services/frame_flush/frame_batcher.h"
#include "views_test_support.h"

namespace {

using eerie_leap::domain::ui_domain::services::frame_flush::Frame;
using eerie_leap::domain::ui_domain::services::frame_flush::FrameBatcher;
using eerie_leap::domain::ui_domain::services::frame_flush::FrameRect;
using eerie_leap::domain::ui_domain::services::frame_flush::IFrameSink;

constexpr int32_t SIZE = 96;
constexpr size_t ARRAY_BYTES = static_cast<size_t>(SIZE) * SIZE * 2;
constexpr size_t GUARD_BYTES = 64;
constexpr uint8_t GUARD = 0x5C;
constexpr uint16_t GARBAGE = 0xBEEF;
// 16 full-width rows, rounded up to a cache line: what the batcher keeps free for the next part.
constexpr size_t MIN_SLOT = 16 * SIZE * 2;

#ifdef CONFIG_LV_COLOR_FORMAT_RGB565_SWAPPED
constexpr lv_color_format_t FORMAT = LV_COLOR_FORMAT_RGB565_SWAPPED;
#else
constexpr lv_color_format_t FORMAT = LV_COLOR_FORMAT_RGB565;
#endif

// Like Zephyr's two arrays, 64-byte aligned, each followed by sentinel bytes that an overrun would change.
struct SlotArray {
    alignas(64) uint8_t data[ARRAY_BYTES];
    uint8_t guard[GUARD_BYTES];
};
static_assert(sizeof(SlotArray) % 64 == 0);
SlotArray slots[2];

size_t RoundUp64(size_t v) {
    return (v + 63) / 64 * 64;
}

size_t RectBytes(const FrameRect& rect) {
    return static_cast<size_t>(rect.w) * rect.h * 2;
}

// Plays the writer: copies each rect into a panel image, checks what the protocol promises about the rects, and
// completes the frame at once or, in deferred mode, through a helper thread after a delay.
class RecordingSink : public IFrameSink {
public:
    struct Record {
        Frame frame;
        std::vector<uint8_t> pixels; // the rects' bytes at Post time, one after the other
    };

    FrameBatcher* batcher = nullptr;
    std::deque<Record> records; // addresses stay valid while it grows
    std::array<uint16_t, SIZE * SIZE> panel{};
    std::vector<uint8_t> fail_indices; // reported failed on every frame
    uint32_t protocol_errors = 0;
    bool deferred = false;

    // Deferred mode.
    k_msgq queue{};
    alignas(Record*) std::array<char, 16 * sizeof(Record*)> queue_storage{};
    volatile uint32_t completed = 0;
    volatile uint32_t changed_in_flight = 0;
    int32_t delay_ms = 0;

    void Reset() {
        records.clear();
        panel.fill(0);
        fail_indices.clear();
        protocol_errors = 0;
        deferred = false;
        completed = 0;
        changed_in_flight = 0;
        k_msgq_init(&queue, queue_storage.data(), sizeof(Record*), queue_storage.size() / sizeof(Record*));
    }

    void Post(Frame& frame) override {
        Record& record = records.emplace_back();
        record.frame = frame;
        CheckRects(frame);
        for(uint8_t i = 0; i < frame.n; ++i) {
            const FrameRect& rect = frame.rects[i];
            const size_t offset = record.pixels.size();
            record.pixels.resize(offset + RectBytes(rect));
            std::memcpy(record.pixels.data() + offset, rect.buf, RectBytes(rect));
            for(uint16_t row = 0; row < rect.h; ++row) {
                std::memcpy(
                    &panel[static_cast<size_t>(rect.y + row) * SIZE + rect.x],
                    rect.buf + static_cast<size_t>(row) * rect.w * 2, static_cast<size_t>(rect.w) * 2
                );
            }
        }

        if(deferred) {
            Record* pointer = &record;
            k_msgq_put(&queue, static_cast<const void*>(&pointer), K_FOREVER);
            return;
        }
        batcher->Complete(record.frame, fail_indices);
        completed = completed + 1;
    }

    // Runs on the helper thread in deferred mode.
    void CompleteLater(Record* record) {
        k_msleep(delay_ms);
        size_t offset = 0;
        for(uint8_t i = 0; i < record->frame.n; ++i) {
            const FrameRect& rect = record->frame.rects[i];
            if(std::memcmp(record->pixels.data() + offset, rect.buf, RectBytes(rect)) != 0)
                changed_in_flight = changed_in_flight + 1;
            offset += RectBytes(rect);
        }
        batcher->Complete(record->frame, fail_indices);
        completed = completed + 1;
    }

private:
    // Every rect: 64-byte aligned, whole inside one array, in the frame's array bits, and clear of the others.
    void CheckRects(const Frame& frame) {
        for(uint8_t i = 0; i < frame.n; ++i) {
            const FrameRect& rect = frame.rects[i];
            const auto start = std::bit_cast<uintptr_t>(rect.buf);
            const auto end = start + RectBytes(rect);
            if(start % 64 != 0 || rect.x + rect.w > SIZE || rect.y + rect.h > SIZE)
                protocol_errors++;
            int array = -1;
            for(int a = 0; a < 2; ++a) {
                const uint8_t* array_start = slots[a].data;
                const auto base = std::bit_cast<uintptr_t>(array_start);
                if(start >= base && end <= base + ARRAY_BYTES)
                    array = a;
            }
            if(array < 0 || (frame.arrays & (1U << array)) == 0)
                protocol_errors++;
            for(uint8_t j = 0; j < i; ++j) {
                const FrameRect& other = frame.rects[j];
                const auto other_start = std::bit_cast<uintptr_t>(other.buf);
                if(start < other_start + RectBytes(other) && other_start < end)
                    protocol_errors++;
            }
        }
    }
};

// RGB565 pixels as little-endian bytes, which is how the targets keep them.
uint8_t image_pixels[16 * 16 * 2];
lv_image_dsc_t image_dsc;

void StubFlush(lv_display_t* disp, const lv_area_t* /*area*/, uint8_t* /*px_map*/) {
    lv_display_flush_ready(disp);
}

void WakeCb(void* ctx) {
    ++*static_cast<uint32_t*>(ctx);
}

struct Rig {
    lv_display_t* disp = nullptr;
    RecordingSink sink;
    FrameBatcher batcher;
    uint32_t wakes = 0;
    uint32_t wait_starts = 0;
    uint32_t wait_finishes = 0;
    lv_obj_t* screen = nullptr;
    lv_obj_t* box_a = nullptr;
    lv_obj_t* box_b = nullptr;
    lv_obj_t* label = nullptr;
    lv_obj_t* image = nullptr;
    std::array<uint16_t, SIZE * SIZE> reference{};
    std::array<uint16_t, SIZE * SIZE> assembled{}; // a copy of the panel: too big for the 8 KB test stack
    uint32_t expected_trips = 0;
    bool active = false;
    bool helper_running = false;
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization): built before the first test runs; an allocation failure there ends the run anyway
Rig rig;
k_thread helper_thread;
K_THREAD_STACK_DEFINE(helper_stack, 4096);

void HelperEntry(void*, void*, void*) {
    for(;;) {
        RecordingSink::Record* record = nullptr;
        k_msgq_get(&rig.sink.queue, static_cast<void*>(&record), K_FOREVER);
        if(record == nullptr)
            return;
        rig.sink.CompleteLater(record);
    }
}

void StopHelper() {
    if(!rig.helper_running)
        return;
    RecordingSink::Record* stop = nullptr;
    k_msgq_put(&rig.sink.queue, static_cast<const void*>(&stop), K_FOREVER);
    k_thread_join(&helper_thread, K_FOREVER);
    rig.helper_running = false;
}

void FlushWaitCb(lv_event_t* e) {
    auto* r = static_cast<Rig*>(lv_event_get_user_data(e));
    if(lv_event_get_code(e) == LV_EVENT_FLUSH_WAIT_START)
        r->wait_starts++;
    else
        r->wait_finishes++;
}

lv_obj_t* MakeBox(lv_obj_t* parent, int32_t w, int32_t h, uint32_t color) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    return box;
}

// A new display with the batcher installed and a small scene, none of it rendered yet.
void Begin() {
    views_test::EnsureTestDisplay();

    for(auto& array : slots) {
        std::memset(array.data, 0x11, sizeof(array.data));
        std::memset(array.guard, GUARD, sizeof(array.guard));
    }
    rig.sink.Reset();
    rig.sink.batcher = &rig.batcher;
    rig.wakes = 0;
    rig.wait_starts = 0;
    rig.wait_finishes = 0;
    rig.expected_trips = 0;

    rig.disp = lv_display_create(SIZE, SIZE);
    zassert_not_null(rig.disp);
    lv_display_set_color_format(rig.disp, FORMAT);
    lv_display_set_buffers(rig.disp, slots[0].data, slots[1].data, ARRAY_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(rig.disp, StubFlush);
    rig.active = true;

    zassert_equal(rig.batcher.Install(rig.disp, rig.sink, WakeCb, &rig.wakes), 0);
    lv_display_add_event_cb(rig.disp, FlushWaitCb, LV_EVENT_FLUSH_WAIT_START, &rig);
    lv_display_add_event_cb(rig.disp, FlushWaitCb, LV_EVENT_FLUSH_WAIT_FINISH, &rig);

    for(size_t i = 0; i < std::size(image_pixels) / 2; ++i) {
        image_pixels[2 * i] = static_cast<uint8_t>((i * 7U) & 0xFFU);
        image_pixels[2 * i + 1] = 0x12;
    }
    image_dsc = {};
    image_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    image_dsc.header.w = 16;
    image_dsc.header.h = 16;
    image_dsc.header.stride = 32;
    image_dsc.data_size = sizeof(image_pixels);
    image_dsc.data = image_pixels;

    rig.screen = lv_display_get_screen_active(rig.disp);
    lv_obj_set_style_bg_color(rig.screen, lv_color_hex(0x203040), 0);
    lv_obj_set_style_bg_opa(rig.screen, LV_OPA_COVER, 0);
    rig.box_a = MakeBox(rig.screen, 30, 20, 0xD02020);
    rig.box_b = MakeBox(rig.screen, 24, 24, 0x20C040);
    rig.label = lv_label_create(rig.screen);
    lv_label_set_text(rig.label, "Hi");
    lv_obj_set_style_text_color(rig.label, lv_color_white(), 0);
    lv_obj_set_pos(rig.label, 10, 70);
    rig.image = lv_image_create(rig.screen);
    lv_image_set_src(rig.image, &image_dsc);
    lv_obj_set_pos(rig.box_a, 4, 4);
    lv_obj_set_pos(rig.box_b, 60, 50);
    lv_obj_set_pos(rig.image, 70, 10);
}

void End() {
    if(!rig.active)
        return;
    StopHelper();
    rig.batcher.Uninstall();
    lv_display_delete(rig.disp);
    rig.disp = nullptr;
    rig.active = false;
}

void After(void* /*fixture*/) {
    End();
}

// Moves the scene a step: the boxes, the image and the label's text all change, so a refresh has several areas.
void Mutate(int step) {
    lv_obj_set_pos(rig.box_a, 4 + step * 6, 4 + step * 3);
    lv_obj_set_pos(rig.box_b, 60 - step * 5, 50 - step * 2);
    lv_obj_set_pos(rig.image, 70 - step * 4, 10 + step * 3);
    lv_label_set_text_fmt(rig.label, "%d", step);
}

bool GuardsIntact() {
    for(const auto& array : slots) {
        for(const uint8_t b : array.guard) {
            if(b != GUARD)
                return false;
        }
    }
    return true;
}

const Frame& LastFrame() {
    return rig.sink.records.back().frame;
}

// One refresh: exactly one post, nothing left flushing, nothing outside the arrays touched.
void Refresh() {
    const size_t before = rig.sink.records.size();
    const uint32_t areas_before = rig.batcher.GetStats().areas;
    lv_refr_now(rig.disp);
    zassert_equal(rig.sink.records.size(), before + 1, "a refresh must post its frame once");
    zassert_equal(rig.disp->flushing, 0, "LVGL must never be left waiting for a flush"); // row 4
    zassert_equal(rig.sink.protocol_errors, 0U);
    zassert_equal(rig.batcher.GetStats().guard_trips, rig.expected_trips);
    zassert_true(GuardsIntact(), "something wrote behind a slot array"); // row 2
    zassert_equal(
        rig.batcher.GetStats().areas - areas_before, static_cast<uint32_t>(LastFrame().n), "one rect per flushed part"
    );
}

// The first refresh: one full-screen area, which is the reference for what every later frame must add up to.
void Settle() {
    Refresh();
    zassert_equal(LastFrame().n, 1);
    zassert_equal(LastFrame().rects[0].w, SIZE);
    zassert_equal(LastFrame().rects[0].h, SIZE);
    rig.reference = rig.sink.panel;
}

// The panel as the frames so far assembled it must equal one full-screen render of the same scene.
void ExpectPanelEqualsFullRender() {
    rig.assembled = rig.sink.panel;
    lv_obj_invalidate(rig.screen);
    Refresh();
    zassert_equal(LastFrame().n, 1);
    zassert_equal(LastFrame().rects[0].w, SIZE);
    zassert_equal(LastFrame().rects[0].h, SIZE);
    size_t wrong = 0;
    for(size_t i = 0; i < rig.assembled.size(); ++i) {
        if(rig.assembled[i] != rig.sink.panel[i]) {
            if(wrong == 0)
                TC_PRINT(
                    "first difference at %u,%u: 0x%04x against 0x%04x\n", (unsigned)(i % SIZE), (unsigned)(i / SIZE),
                    rig.assembled[i], rig.sink.panel[i]
                );
            ++wrong;
        }
    }
    zassert_equal(wrong, 0U, "the frames do not add up to a full render");
}

void Invalidate(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    lv_area_t area;
    lv_area_set(&area, x1, y1, x2, y2);
    lv_inv_area(rig.disp, &area);
}

// The union of the frame's rects must hold the reference and nothing else may have been written: the panel was
// filled with garbage first.
void ExpectOnlyRectsWritten(const Frame& frame) {
    for(int32_t y = 0; y < SIZE; ++y) {
        for(int32_t x = 0; x < SIZE; ++x) {
            bool inside = false;
            for(uint8_t i = 0; i < frame.n; ++i) {
                const FrameRect& r = frame.rects[i];
                const int32_t rx = r.x;
                const int32_t ry = r.y;
                inside = inside || (x >= rx && x < rx + r.w && y >= ry && y < ry + r.h);
            }
            const uint16_t got = rig.sink.panel[static_cast<size_t>(y) * SIZE + x];
            const uint16_t expected = inside ? rig.reference[static_cast<size_t>(y) * SIZE + x] : GARBAGE;
            zassert_equal(got, expected, "pixel %d,%d is 0x%04x, expected 0x%04x", (int)x, (int)y, got, expected);
        }
    }
}

// Three areas LVGL keeps apart (a join needs the joined area to be smaller than the two together): the halves
// of the screen and a band across the middle that overlaps both.
void InvalidateHalvesAndBand(int32_t band_y1, int32_t band_y2) {
    Invalidate(0, 0, 47, 95);
    Invalidate(48, 0, 95, 95);
    Invalidate(0, band_y1, 95, band_y2);
}

// Left half, a band, right half: the band fills the rest of the first array up to 3,456 bytes short of the right
// half, which is more than the look-ahead's floor but less than the half.
void InvalidateHalfBandHalf() {
    Invalidate(0, 0, 47, 95);
    Invalidate(0, 30, 95, 59);
    Invalidate(48, 0, 95, 95);
}

// Completes every frame after `delay_ms` on the helper thread, so a batch stays in flight while the next renders.
void StartDeferredSink(int32_t delay_ms) {
    rig.sink.deferred = true;
    rig.sink.delay_ms = delay_ms;
    k_thread_create(
        &helper_thread, helper_stack, K_THREAD_STACK_SIZEOF(helper_stack), HelperEntry, nullptr, nullptr, nullptr,
        K_PRIO_COOP(2), 0, K_NO_WAIT
    );
    rig.helper_running = true;
}

// Waits until the helper has completed every posted frame, stops the helper and goes back to the synchronous sink.
void DrainDeferredSink() {
    for(int i = 0; i < 100 && rig.sink.completed < rig.sink.records.size(); ++i)
        k_msleep(20);
    zassert_equal(rig.sink.completed, rig.sink.records.size());
    zassert_equal(rig.sink.changed_in_flight, 0U, "a posted frame's bytes changed before it was complete");
    zassert_equal(rig.sink.protocol_errors, 0U);
    zassert_true(GuardsIntact());
    StopHelper();
    rig.sink.deferred = false;
}

} // namespace

ZTEST(frame_batcher, test_install_changes_nothing_when_a_check_fails) {
    views_test::EnsureTestDisplay();
    rig.sink.Reset();
    rig.sink.batcher = &rig.batcher;

    // A display with one buffer, and one whose arrays are not cache line aligned: both refused, both untouched.
    lv_display_t* single = lv_display_create(SIZE, SIZE);
    zassert_not_null(single);
    lv_display_set_color_format(single, FORMAT);
    lv_display_set_buffers(single, slots[0].data, nullptr, ARRAY_BYTES, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(single, StubFlush);
    lv_draw_buf_t* const buf_before = single->buf_1;
    const auto flush_before = lv_display_get_flush_cb(single);

    uint32_t wakes = 0;
    FrameBatcher refused;
    zassert_equal(refused.Install(single, rig.sink, WakeCb, &wakes), -EINVAL);
    zassert_equal(refused.GetStats().install_failed, 1U);
    zassert_equal(single->buf_1, buf_before);
    zassert_is_null(single->buf_2);
    zassert_equal(lv_display_get_flush_cb(single), flush_before);
    zassert_is_null(FrameBatcher::Instance());

    lv_display_set_buffers(single, slots[0].data + 4, slots[1].data, ARRAY_BYTES - 4, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_draw_buf_t* const aligned_before = single->buf_1;
    zassert_equal(refused.Install(single, rig.sink, WakeCb, &wakes), -EINVAL);
    zassert_equal(refused.GetStats().install_failed, 2U);
    zassert_equal(single->buf_1, aligned_before);
    zassert_equal(single->buf_act, single->buf_1);
    zassert_equal(lv_display_get_flush_cb(single), flush_before);
    zassert_is_null(FrameBatcher::Instance());
    lv_display_delete(single);
}

ZTEST(frame_batcher, test_uninstall_gives_the_display_back_its_buffers_and_flush_callback) {
    Begin();
    zassert_equal(FrameBatcher::Instance(), &rig.batcher);
    zassert_false(lv_display_is_double_buffered(rig.disp));
    zassert_equal(rig.disp->buf_act->data, slots[0].data); // the first slot

    rig.batcher.Uninstall();
    zassert_is_null(FrameBatcher::Instance());
    zassert_true(lv_display_is_double_buffered(rig.disp));
    zassert_equal(rig.disp->buf_1->data, slots[0].data);
    zassert_equal(rig.disp->buf_2->data, slots[1].data);
    zassert_equal(lv_display_get_flush_cb(rig.disp), StubFlush);
    // LVGL renders on its own again.
    lv_refr_now(rig.disp);
    zassert_equal(rig.sink.records.size(), 0U);
}

ZTEST(frame_batcher, test_frames_add_up_to_a_full_render_bit_for_bit) {
    Begin();
    Settle();
    int most_rects = 0;
    for(int step = 1; step <= 4; ++step) {
        Mutate(step);
        Refresh();
        most_rects = std::max<int>(most_rects, LastFrame().n);
    }
    zassert_true(most_rects >= 2, "the scene's changes must give a frame with several areas");
    ExpectPanelEqualsFullRender();
    zassert_equal(rig.batcher.GetStats().array_waits, 0U);
    zassert_equal(rig.batcher.GetStats().overflows, 0U);
}

// Rows 4, 5, 8: single-buffered, one flush-last per refresh, the areas back to back in one array.
ZTEST(frame_batcher, test_one_post_per_refresh_with_the_areas_packed_back_to_back) {
    Begin();
    Settle();
    // Three separate boxes: the cursor must advance by each area rounded up to 64 bytes.
    Invalidate(2, 2, 21, 11);
    Invalidate(30, 40, 69, 44);
    Invalidate(80, 70, 90, 90);
    Refresh();
    const Frame& frame = LastFrame();
    zassert_equal(frame.n, 3);
    zassert_false(lv_display_is_double_buffered(rig.disp));
    zassert_equal(frame.arrays, 2); // the settle frame used array 0
    const uint8_t* cursor = slots[1].data;
    for(uint8_t i = 0; i < frame.n; ++i) {
        zassert_equal(frame.rects[i].buf, cursor, "rect %d is not where the previous one ended", (int)i);
        cursor += RoundUp64(RectBytes(frame.rects[i]));
    }
    // LVGL itself brackets a wait before every area it renders (row 4), and has nothing to wait for.
    zassert_equal(rig.wait_starts, rig.batcher.GetStats().areas);
    zassert_equal(rig.wait_finishes, rig.wait_starts);
    zassert_equal(rig.batcher.GetStats().array_waits, 0U);
}

// Row 9, and the batch in flight: the renderer blocks at the third frame's RENDER_START until the first one is
// on the panel; what a posted frame holds does not change while the next ones render.
ZTEST(frame_batcher, test_the_renderer_waits_for_a_batch_in_flight_only_when_a_frame_renders) {
    Begin();
    rig.sink.deferred = true;
    rig.sink.delay_ms = 80;
    k_thread_create(
        &helper_thread, helper_stack, K_THREAD_STACK_SIZEOF(helper_stack), HelperEntry, nullptr, nullptr, nullptr,
        K_PRIO_COOP(2), 0, K_NO_WAIT
    );
    rig.helper_running = true;

    Settle(); // frame 1, array 0
    const int64_t posted_at = k_uptime_get();
    Mutate(1);
    Refresh(); // frame 2, array 1: nothing to wait for
    zassert_equal(rig.batcher.GetStats().array_waits, 0U);

    // Both batches are outstanding and the renderer is idle: nothing blocks without a frame to render.
    zassert_equal(rig.sink.completed, 0U);

    Mutate(2);
    Refresh(); // frame 3 needs array 0 back
    const int64_t waited = k_uptime_get() - posted_at;
    zassert_true(waited >= 60, "frame 3 rendered after %d ms, before frame 1 was written", (int)waited);
    const auto stats = rig.batcher.GetStats();
    zassert_equal(stats.array_waits, 1U);
    zassert_true(stats.array_wait_us >= 20000U, "waited %u us", (unsigned)stats.array_wait_us);
    zassert_equal(stats.array_wait_max_us, static_cast<uint32_t>(stats.array_wait_us));
    // The wait is bracketed so LVGL's performance monitor counts it as flush, on top of LVGL's own bracket before
    // each area.
    zassert_equal(rig.wait_starts, stats.areas + 1);
    zassert_equal(rig.wait_finishes, rig.wait_starts);
    zassert_equal(LastFrame().rects[0].buf, slots[0].data);

    for(int i = 0; i < 100 && rig.sink.completed < rig.sink.records.size(); ++i)
        k_msleep(20);
    zassert_equal(rig.sink.completed, rig.sink.records.size());
    zassert_equal(rig.sink.changed_in_flight, 0U, "a posted frame's bytes changed before it was complete");
    zassert_equal(rig.sink.protocol_errors, 0U);
    zassert_true(GuardsIntact());

    rig.sink.deferred = false;
    ExpectPanelEqualsFullRender();
}

// Row 8: the next area is the next unjoined entry of inv_areas, and it renders as one part in the other array.
ZTEST(frame_batcher, test_the_look_ahead_switches_arrays_before_an_area_that_does_not_fit) {
    Begin();
    Settle();
    rig.sink.panel.fill(GARBAGE);
    InvalidateHalfBandHalf();
    Refresh();
    const Frame& frame = LastFrame();
    zassert_equal(frame.n, 3, "the right half must render as one part");
    zassert_equal(frame.arrays, 3);
    zassert_equal(frame.rects[0].buf, slots[1].data); // the settle frame used array 0
    zassert_equal(frame.rects[1].buf, slots[1].data + 9216);
    zassert_equal(frame.rects[2].buf, slots[0].data);
    zassert_equal(frame.rects[2].h, SIZE);
    const auto stats = rig.batcher.GetStats();
    zassert_equal(stats.lookahead_switches, 1U);
    zassert_equal(stats.overflows, 1U);
    zassert_equal(stats.frame_breaks, 0U);
    ExpectOnlyRectsWritten(frame);
}

// Three areas that are 1.2 arrays together: the frame spans both arrays and the content is right.
ZTEST(frame_batcher, test_a_frame_of_more_than_one_array_continues_in_the_other) {
    Begin();
    Settle();
    rig.sink.panel.fill(GARBAGE);
    InvalidateHalvesAndBand(38, 57);
    Refresh();
    const Frame& frame = LastFrame();
    zassert_equal(frame.n, 3);
    zassert_equal(frame.arrays, 3);
    zassert_equal(frame.rects[0].buf, slots[1].data);
    zassert_equal(frame.rects[1].buf, slots[1].data + 9216);
    zassert_equal(frame.rects[2].buf, slots[0].data);
    zassert_equal(rig.batcher.GetStats().overflows, 1U);
    zassert_equal(rig.batcher.GetStats().frame_breaks, 0U);
    ExpectOnlyRectsWritten(frame);

    // The next frame has to wait for this whole batch; here it is complete at once, so it does not wait.
    Mutate(1);
    Refresh();
    zassert_equal(rig.batcher.GetStats().array_waits, 0U);
    ExpectPanelEqualsFullRender();
}

// Row 3: parts are sized once per area from the room at its start. With the look-ahead off the right half is cut
// where the first array ends.
ZTEST(frame_batcher, test_an_area_that_does_not_fit_is_cut_into_parts_without_the_look_ahead) {
    Begin();
    Settle();
    rig.batcher.SetLookAheadForTest(false);
    rig.sink.panel.fill(GARBAGE);
    InvalidateHalfBandHalf();
    Refresh();
    const Frame& frame = LastFrame();
    zassert_equal(frame.n, 5);
    // 3,456 bytes of room are 36 rows of the half's 96 bytes.
    zassert_equal(frame.rects[2].buf, slots[1].data + 14976);
    zassert_equal(frame.rects[2].y, 0);
    zassert_equal(frame.rects[2].h, 36);
    zassert_equal(frame.rects[3].buf, slots[0].data);
    zassert_equal(frame.rects[3].y, 36);
    zassert_equal(frame.rects[3].h, 36);
    zassert_equal(frame.rects[4].buf, slots[0].data + 3456);
    zassert_equal(frame.rects[4].y, 72);
    zassert_equal(frame.rects[4].h, 24);
    zassert_equal(rig.batcher.GetStats().lookahead_switches, 0U);
    zassert_equal(rig.batcher.GetStats().overflows, 1U);
    zassert_equal(rig.batcher.GetStats().guard_trips, 0U);
    ExpectOnlyRectsWritten(frame);
    zassert_true(GuardsIntact());
    zassert_true(MIN_SLOT <= 3456);
}

// A frame that needs more than both arrays: what is rendered goes out as a batch, the renderer waits until it is
// written and the frame goes on from the start of an array.
ZTEST(frame_batcher, test_a_frame_of_more_than_two_arrays_is_posted_in_pieces) {
    Begin();
    Settle();
    rig.sink.panel.fill(GARBAGE);
    // Two bands across and two columns down, each just under half a screen, and a box in the middle: 1.1 times two
    // arrays, and every pair of them stays apart.
    Invalidate(0, 0, 95, 46);
    Invalidate(0, 49, 95, 95);
    Invalidate(0, 0, 46, 95);
    Invalidate(49, 0, 95, 95);
    Invalidate(24, 24, 71, 71);
    const size_t before = rig.sink.records.size();
    lv_refr_now(rig.disp);
    zassert_true(rig.sink.records.size() >= before + 2, "the frame must have been posted in pieces");
    zassert_true(rig.batcher.GetStats().frame_breaks >= 1U);
    zassert_equal(rig.sink.protocol_errors, 0U);
    zassert_true(GuardsIntact());
    zassert_equal(rig.disp->flushing, 0);
    // Whatever order the pieces went in, the panel holds the reference.
    for(size_t i = 0; i < rig.sink.panel.size(); ++i) {
        const uint16_t got = rig.sink.panel[i];
        zassert_true(got == rig.reference[i] || got == GARBAGE, "pixel %u is 0x%04x", (unsigned)i, got);
    }
    ExpectPanelEqualsFullRender();
}

ZTEST(frame_batcher, test_a_failed_rect_is_invalidated_even_when_the_refresh_timer_is_paused) {
    Begin();
    Settle();
    rig.sink.fail_indices = { 0 };
    Mutate(1);
    Refresh();
    zassert_equal(rig.wakes, 1U);
    const FrameRect failed = LastFrame().rects[0];
    rig.sink.fail_indices.clear();

    // LVGL leaves the timer paused after a refresh when the performance monitor is off.
    lv_timer_t* refresh_timer = lv_display_get_refr_timer(rig.disp);
    lv_timer_pause(refresh_timer);
    zassert_true(lv_timer_get_paused(refresh_timer));
    rig.batcher.ApplyRepairs();
    zassert_false(lv_timer_get_paused(refresh_timer), "the repair must resume the refresh timer");
    zassert_equal(rig.batcher.GetStats().repairs, 1U);

    Refresh();
    bool covered = false;
    for(uint8_t i = 0; i < LastFrame().n; ++i) {
        const FrameRect& r = LastFrame().rects[i];
        covered = covered
            || (r.x <= failed.x
                && r.y <= failed.y
                && r.x + r.w >= failed.x + failed.w
                && r.y + r.h >= failed.y + failed.h);
    }
    zassert_true(covered, "the next frame does not contain the failed rect");
    // Nothing more to repair.
    rig.batcher.ApplyRepairs();
    zassert_equal(rig.batcher.GetStats().repairs, 1U);
}

// Row 9 again, from inside the flush callback: an overflow into the array a batch still holds waits for it. The
// wait is not bracketed for LVGL's monitor there (the callback counts as flush already), the batcher's own counters
// see it.
ZTEST(frame_batcher, test_an_overflow_waits_for_the_array_of_a_batch_in_flight) {
    Begin();
    StartDeferredSink(80);
    Settle(); // array 0, in flight for 80 ms
    rig.sink.panel.fill(GARBAGE);
    InvalidateHalfBandHalf();
    Refresh(); // starts in array 1 and overflows into array 0
    const auto stats = rig.batcher.GetStats();
    zassert_equal(stats.overflows, 1U);
    zassert_equal(stats.array_waits, 1U);
    zassert_true(stats.array_wait_us >= 20000U, "waited %u us", (unsigned)stats.array_wait_us);
    zassert_equal(rig.wait_starts, stats.areas, "only LVGL's own bracket per area");
    zassert_equal(rig.wait_finishes, rig.wait_starts);
    DrainDeferredSink();
    ExpectPanelEqualsFullRender();
}

// The same for the look-ahead's switch and for a frame break, which waits for both arrays.
ZTEST(frame_batcher, test_a_look_ahead_switch_and_a_frame_break_wait_for_the_batches_in_flight) {
    Begin();
    StartDeferredSink(80);
    Settle();
    InvalidateHalvesAndBand(38, 57);
    Refresh();
    zassert_equal(rig.batcher.GetStats().array_waits, 1U);
    zassert_equal(rig.batcher.GetStats().lookahead_switches, 1U);
    DrainDeferredSink();

    StartDeferredSink(80);
    const uint32_t waits_before = rig.batcher.GetStats().array_waits;
    Invalidate(0, 0, 95, 46);
    Invalidate(0, 49, 95, 95);
    Invalidate(0, 0, 46, 95);
    Invalidate(49, 0, 95, 95);
    Invalidate(24, 24, 71, 71);
    lv_refr_now(rig.disp);
    const auto stats = rig.batcher.GetStats();
    zassert_true(stats.frame_breaks >= 1U);
    zassert_true(stats.array_waits > waits_before, "a break must wait for the arrays");
    zassert_equal(rig.wait_starts, stats.areas + 0U, "only LVGL's own bracket per area");
    DrainDeferredSink();
    zassert_equal(rig.disp->flushing, 0);
    ExpectPanelEqualsFullRender();
}

// A small area after a big one: the look-ahead sizes the next area, not the one just finished.
ZTEST(frame_batcher, test_the_look_ahead_sizes_the_next_area_and_not_the_one_just_finished) {
    Begin();
    Settle();
    rig.sink.panel.fill(GARBAGE);
    Invalidate(0, 0, 95, 59);  // 11,520 bytes
    Invalidate(0, 70, 19, 89); // 800 bytes: fits behind the band, would not behind another band
    Refresh();
    const Frame& frame = LastFrame();
    zassert_equal(frame.n, 2);
    zassert_equal(frame.rects[0].buf, slots[1].data);
    zassert_equal(frame.rects[1].buf, slots[1].data + 11520);
    zassert_equal(rig.batcher.GetStats().lookahead_switches, 0U);
    zassert_equal(rig.batcher.GetStats().overflows, 0U);
    ExpectOnlyRectsWritten(frame);
}

// The full-screen repair frame fails too: a small clean frame after it says nothing about the screen, so the repair
// stays wanted until a clean frame follows a repair that went out.
ZTEST(frame_batcher, test_a_clean_small_frame_after_a_failed_repair_does_not_end_the_deferral) {
    Begin();
    Settle();
    rig.sink.fail_indices = { 0 };
    for(int step = 1; step <= 3; ++step) {
        Mutate(step);
        Refresh();
    }
    zassert_true(rig.batcher.IsRepairDeferred());
    rig.batcher.ApplyRepairs();
    Refresh(); // the full-screen repair frame fails
    zassert_true(rig.batcher.IsRepairDeferred());

    rig.sink.fail_indices.clear();
    Mutate(4);
    Refresh(); // clean, but only a small update
    zassert_true(rig.batcher.IsRepairDeferred(), "a small clean frame must not end the deferral");

    lv_timer_t* repair_timer = rig.batcher.GetRepairTimer();
    zassert_not_null(repair_timer);
    repair_timer->timer_cb(repair_timer);
    Refresh(); // the next full-screen repair lands
    zassert_false(rig.batcher.IsRepairDeferred());
    repair_timer->timer_cb(repair_timer);
    zassert_is_null(rig.batcher.GetRepairTimer());
    ExpectPanelEqualsFullRender();
}

ZTEST(frame_batcher, test_three_failing_frames_in_a_row_become_one_deferred_full_screen_repair) {
    Begin();
    Settle();
    rig.sink.fail_indices = { 0 };
    for(int step = 1; step <= 3; ++step) {
        Mutate(step);
        Refresh();
    }
    zassert_true(rig.batcher.IsRepairDeferred());
    zassert_equal(rig.wakes, 3U);

    lv_timer_t* refresh_timer = lv_display_get_refr_timer(rig.disp);
    lv_timer_pause(refresh_timer);
    rig.batcher.ApplyRepairs();
    zassert_false(lv_timer_get_paused(refresh_timer));
    zassert_equal(rig.batcher.GetStats().repairs, 0U, "the third frame folds the earlier repairs into the full one");
    zassert_true(rig.batcher.GetStats().repairs_deferred >= 3U);
    lv_timer_t* repair_timer = rig.batcher.GetRepairTimer();
    zassert_not_null(repair_timer);
    zassert_equal(repair_timer->period, 1000U);

    // The full-screen repair frame fails again: the timer asks for another one a second later.
    Refresh();
    zassert_equal(LastFrame().n, 1);
    zassert_equal(LastFrame().rects[0].w, SIZE);
    zassert_equal(LastFrame().rects[0].h, SIZE);
    zassert_true(rig.batcher.IsRepairDeferred());
    lv_timer_pause(refresh_timer);
    repair_timer->timer_cb(repair_timer);
    zassert_false(lv_timer_get_paused(refresh_timer));

    // A frame that completes without a failure ends it, and the timer deletes itself on its next run.
    rig.sink.fail_indices.clear();
    Refresh();
    zassert_equal(LastFrame().rects[0].w, SIZE);
    zassert_false(rig.batcher.IsRepairDeferred());
    repair_timer->timer_cb(repair_timer);
    zassert_is_null(rig.batcher.GetRepairTimer());
    ExpectPanelEqualsFullRender();
}

ZTEST(frame_batcher, test_a_frame_that_completes_cleanly_resets_the_count_of_failing_frames) {
    Begin();
    Settle();
    for(int step = 1; step <= 6; ++step) {
        rig.sink.fail_indices.clear();
        if(step % 3 != 0)
            rig.sink.fail_indices = { 0 };
        Mutate(step);
        Refresh();
    }
    zassert_false(rig.batcher.IsRepairDeferred());
    zassert_is_null(rig.batcher.GetRepairTimer());
}

// The guard: a slot header that does not match the area. The header is changed in FLUSH_START, which LVGL sends
// right before the flush callback.
ZTEST(frame_batcher, test_a_slot_header_that_does_not_match_trips_the_guard_and_serial_mode_keeps_the_content) {
    Begin();
    Settle();

    struct Spoil {
        lv_display_t* disp;
        bool done;
    } spoil{ rig.disp, false };
    auto spoil_cb = [](lv_event_t* e) {
        auto* s = static_cast<Spoil*>(lv_event_get_user_data(e));
        if(!s->done) {
            s->done = true;
            s->disp->buf_act->header.w += 1;
        }
    };
    lv_display_add_event_cb(rig.disp, spoil_cb, LV_EVENT_FLUSH_START, &spoil);

    const size_t before = rig.sink.records.size();
    lv_obj_set_style_bg_color(rig.box_a, lv_color_hex(0x2020D0), 0); // one area
    lv_refr_now(rig.disp);
    zassert_true(spoil.done);
    zassert_equal(rig.batcher.GetStats().guard_trips, 1U);
    zassert_equal(rig.sink.records.size(), before, "the suspect area must not be posted");
    zassert_equal(rig.disp->flushing, 0);
    zassert_true(rig.wakes >= 1U, "the full-screen repair needs the renderer to run again");

    rig.expected_trips = 1;
    // The repair is one full-screen area, posted on its own and waited for.
    rig.batcher.ApplyRepairs();
    lv_refr_now(rig.disp);
    zassert_equal(rig.sink.records.size(), before + 1);
    zassert_equal(LastFrame().n, 1);
    zassert_equal(LastFrame().rects[0].w, SIZE);
    zassert_equal(LastFrame().rects[0].h, SIZE);

    // Serial mode: every area its own batch from here on, at the start of one array.
    Mutate(2);
    const size_t serial_before = rig.sink.records.size();
    lv_refr_now(rig.disp);
    zassert_true(rig.sink.records.size() >= serial_before + 1);
    for(size_t i = serial_before; i < rig.sink.records.size(); ++i) {
        zassert_equal(rig.sink.records[i].frame.n, 1);
    }
    zassert_equal(rig.batcher.GetStats().guard_trips, 1U);
    zassert_equal(rig.sink.protocol_errors, 0U);
    zassert_true(GuardsIntact());
    ExpectPanelEqualsFullRender();
}

#ifdef CONFIG_GAUGE_ROUND_PANEL_CLIP

namespace {

constexpr int32_t MARGIN_PX = CONFIG_GAUGE_ROUND_PANEL_CLIP_MARGIN_PX;
constexpr uint16_t SENTINEL = 0xA55A; // not byte-symmetric, so a swapped sentinel differs from it

uint16_t SourcePixel(int32_t x, int32_t y) {
    return static_cast<uint16_t>(0x1200U | ((x * 5 + y * 3) & 0xFFU));
}

// As in gauge_draw_sw.cpp, for this display's size: the pixels a circle inscribed in the square touches.
bool TouchesCircle(int32_t x, int32_t y) {
    const double centre = SIZE / 2.0;
    auto nearest = [centre](int32_t p) {
        const double d = std::fabs(p + 0.5 - centre) - 0.5;
        return d > 0 ? d : 0.0;
    };
    const double dx = nearest(x);
    const double dy = nearest(y);
    return dx * dx + dy * dy < centre * centre;
}

int32_t ExpectedFrom(int32_t y) {
    const int32_t a = std::abs(2 * y + 1 - SIZE);
    const int32_t e = a > 1 ? a - 1 : 0;
    const int32_t v = SIZE * SIZE - e * e;
    int32_t k = 0;
    while(v > 0 && 4 * k * k < v) {
        ++k;
    }
    return std::max<int32_t>(0, (SIZE - 2 * (k + MARGIN_PX)) / 2);
}

} // namespace

// The hook skips the corners of a blit that fills the display's whole active frame buffer. A full-screen area
// sits at offset 0 of an array and is the frame's only area, so the skip must still engage (plan section 4.7).
ZTEST(frame_batcher, test_a_full_screen_image_still_leaves_the_corners_of_its_slot_alone) {
    Begin();
    static uint8_t pixels[SIZE * SIZE * 2];
    static lv_image_dsc_t dsc;
    for(int32_t y = 0; y < SIZE; ++y) {
        for(int32_t x = 0; x < SIZE; ++x) {
            const uint16_t px = SourcePixel(x, y);
            pixels[(y * SIZE + x) * 2] = static_cast<uint8_t>(px & 0xFFU);
            pixels[(y * SIZE + x) * 2 + 1] = static_cast<uint8_t>(px >> 8);
        }
    }
    dsc = {};
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    dsc.header.w = SIZE;
    dsc.header.h = SIZE;
    dsc.header.stride = SIZE * 2;
    dsc.data_size = sizeof(pixels);
    dsc.data = pixels;
    lv_obj_set_style_bg_opa(rig.screen, LV_OPA_TRANSP, 0);
    lv_obj_delete(rig.box_a);
    lv_obj_delete(rig.box_b);
    lv_obj_delete(rig.label);
    lv_obj_delete(rig.image);
    rig.box_a = rig.box_b = rig.label = rig.image = nullptr;
    lv_obj_t* full = lv_image_create(rig.screen);
    lv_image_set_src(full, &dsc);
    lv_obj_set_pos(full, 0, 0);

    // The first slot is the active buffer, and the corners keep whatever was there.
    zassert_equal(rig.disp->buf_act->data, slots[0].data);
    for(size_t i = 0; i + 1 < ARRAY_BYTES; i += 2) {
        std::memcpy(slots[0].data + i, &SENTINEL, sizeof(SENTINEL));
    }
    lv_obj_invalidate(rig.screen);
    lv_refr_now(rig.disp);
    zassert_equal(LastFrame().n, 1);
    zassert_equal(LastFrame().rects[0].buf, slots[0].data);

    for(int32_t y = 0; y < SIZE; ++y) {
        const int32_t from = ExpectedFrom(y);
        for(int32_t x = 0; x < SIZE; ++x) {
            uint16_t got;
            std::memcpy(&got, slots[0].data + (static_cast<size_t>(y) * SIZE + x) * 2, sizeof(got));
            if(x >= from && x < SIZE - from) {
                zassert_equal(got, std::rotl(SourcePixel(x, y), 8), "pixel %d,%d is 0x%04x", (int)x, (int)y, got);
            } else {
                zassert_false(TouchesCircle(x, y), "visible pixel %d,%d outside the copied range", (int)x, (int)y);
                zassert_equal(got, SENTINEL, "pixel %d,%d outside the chord was written", (int)x, (int)y);
            }
        }
    }
    zassert_true(GuardsIntact());
}

#endif // CONFIG_GAUGE_ROUND_PANEL_CLIP

ZTEST_SUITE(frame_batcher, NULL, NULL, NULL, After, NULL);
