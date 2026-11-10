#ifdef CONFIG_EERIE_LEAP_UI_FRAME_FLUSH

    #include <atomic>
    #include <cerrno>
    #include <exception>

    #include <lvgl.h>
    #include <zephyr/device.h>
    #include <zephyr/drivers/display.h>
    #include <zephyr/drivers/display/display_co5300.h>
    #include <zephyr/kernel.h>
    #include <zephyr/logging/log.h>

    #include "eerie_memory.hpp"
    #include "subsys/device_tree/dt_display.h"
    #include "utilities/memory/memory_resource_manager.h"

    #include "co5300_frame_writer.h"

// The slots are contiguous (pitch equals width) only while LVGL's stride is w * 2, and the slot arrays are 64-byte
// aligned (see FrameBatcher::Install(), which refuses at run time what these cannot see).
static_assert(LV_DRAW_BUF_STRIDE_ALIGN == 1, "the frame writer needs rows back to back");
static_assert(64 % LV_DRAW_BUF_ALIGN == 0, "LV_DRAW_BUF_ALIGN must divide the 64-byte rect alignment");
static_assert(CONFIG_LV_Z_VDB_ALIGN % 64 == 0, "the frame buffers must start on a cache line");
static_assert(
    eerie_leap::domain::ui_domain::services::frame_flush::kMaxRects <= CONFIG_ELCO5300_FRAME_MAX_RECTS,
    "the driver must take every rect LVGL can invalidate"
);

namespace eerie_leap::domain::ui_domain::services::frame_flush {

using eerie_leap::subsys::device_tree::DtDisplay;
using eerie_leap::utilities::memory::Mrm;

LOG_MODULE_REGISTER(co5300_frame_writer_logger);

namespace {

// Two frames can be outstanding at most: the one being written and the one just posted. Post() waits forever anyway,
// so a slip in the protocol stalls the renderer until the writer dequeues, and never drops a frame whose arrays
// nobody would give back.
K_MSGQ_DEFINE(frame_queue, sizeof(Frame*), 2, alignof(Frame*));

eerie_memory::PmrUniquePtr<co5300_frame> driver_frame_;
std::atomic<bool> batching_ = true;

    #ifdef CONFIG_ELCO5300_STATS
std::atomic<int> renderer_priority_ = 0;
    #endif

class FrameSink : public IFrameSink {
public:
    void Post(Frame& frame) override {
        Frame* pointer = &frame;

    #ifdef CONFIG_ELCO5300_STATS
        // One stamp per batch: co5300_write_frame() turns it into the post-to-start sample on entry.
        co5300_stats_mark_post(
            DtDisplay::Get(), k_cycle_get_32(), k_thread_priority_get(k_current_get()) < renderer_priority_.load()
        );
    #endif

        k_msgq_put(&frame_queue, &pointer, K_FOREVER);
    }
};

FrameSink sink_;

// Fills the driver's frame from the app's. The pixels stay where LVGL rendered them.
void ConvertFrame(const Frame& frame, co5300_frame& out) {
    out.n = frame.n;
    for(uint8_t i = 0; i < frame.n; ++i) {
        const FrameRect& rect = frame.rects[i];
        out.rects[i] = co5300_rect{ rect.buf, rect.x, rect.y, rect.w, rect.h };
    }
}

// Bench diagnostic: one display_write() per rect, in LVGL's order. Returns the number of rects that failed.
uint8_t WriteEachRect(const device* display, const Frame& frame, uint8_t* failed) {
    uint8_t failed_count = 0;
    for(uint8_t i = 0; i < frame.n; ++i) {
        const FrameRect& rect = frame.rects[i];
        display_buffer_descriptor desc{};
        desc.buf_size = static_cast<size_t>(rect.w) * rect.h * 2U;
        desc.width = rect.w;
        desc.height = rect.h;
        desc.pitch = rect.w;
        desc.frame_incomplete = i + 1U < frame.n;

        if(display_write(display, rect.x, rect.y, &desc, rect.buf) != 0)
            failed[failed_count++] = i;
    }
    return failed_count;
}

void WriteFrame(const Frame& frame) {
    const device* display = DtDisplay::Get();
    uint8_t failed_all[kMaxRects];
    uint8_t* failed = failed_all;
    uint8_t failed_count = 0;

    if(batching_.load(std::memory_order_relaxed) && driver_frame_ != nullptr) {
        co5300_frame& out = *driver_frame_;
        ConvertFrame(frame, out);

        const int ret = co5300_write_frame(display, &out);
        if(ret == -EINVAL) {
            // Refused before anything was sent, and the driver left its failure list alone.
            LOG_ERR("The driver refused a frame of %u rects", static_cast<unsigned>(frame.n));
            for(uint8_t i = 0; i < frame.n; ++i)
                failed[failed_count++] = i;
        } else {
            failed = out.failed_index;
            failed_count = out.failed;
            if(ret != 0 && failed_count == 0) {
                // An error without a list: nothing says what reached the panel, so all of it is repaired.
                failed = failed_all;
                for(uint8_t i = 0; i < frame.n; ++i)
                    failed[failed_count++] = i;
            }
        }
    } else {
        failed_count = WriteEachRect(display, frame, failed);
    }

    // Always, whatever the result: the slot arrays go back to the renderer here.
    FrameBatcher* batcher = FrameBatcher::Instance();
    if(batcher != nullptr)
        batcher->Complete(frame, std::span<const uint8_t>(failed, failed_count));
}

} // namespace

int Co5300WriterInitialize() {
    if(driver_frame_ != nullptr)
        return 0;

    try {
        driver_frame_ = eerie_memory::make_unique_pmr<co5300_frame>(Mrm::GetExtPmr());
    } catch(const std::exception&) {
        LOG_ERR("No memory for the driver frame");
        return -ENOMEM;
    }
    return 0;
}

IFrameSink& Co5300FrameSink() {
    return sink_;
}

void SetBatching(bool enabled) {
    batching_.store(enabled);
}

bool IsBatching() {
    return batching_.load();
}

    #ifdef CONFIG_ELCO5300_STATS
void SetRendererPriority(int priority) {
    renderer_priority_.store(priority);
}
    #endif

// Never takes the LVGL lock: the renderer can be waiting for this thread while it holds it.
void WriterRun() {
    for(;;) {
        Frame* frame = nullptr;
        k_msgq_get(&frame_queue, &frame, K_FOREVER);
        WriteFrame(*frame);
    }
}

} // namespace eerie_leap::domain::ui_domain::services::frame_flush

namespace {

void WriterEntry(void*, void*, void*) {
    eerie_leap::domain::ui_domain::services::frame_flush::WriterRun();
}

} // namespace

// Cooperative and above the bus workers, like Zephyr's flush thread before it. The static stack only moves.
K_THREAD_DEFINE(
    ui_frame_writer,
    CONFIG_EERIE_LEAP_UI_FRAME_FLUSH_THREAD_STACK_SIZE,
    WriterEntry,
    nullptr,
    nullptr,
    nullptr,
    CONFIG_EERIE_LEAP_UI_FRAME_FLUSH_THREAD_PRIORITY,
    0,
    0
);

namespace eerie_leap::domain::ui_domain::services::frame_flush {

k_tid_t WriterTid() {
    return ui_frame_writer;
}

} // namespace eerie_leap::domain::ui_domain::services::frame_flush

#endif // CONFIG_EERIE_LEAP_UI_FRAME_FLUSH
