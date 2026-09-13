#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <lvgl_mem.h>

#ifdef CONFIG_ESP32_PPA
#include <zephyr/cache.h>
#include <driver/ppa.h>
#endif

#include "utilities/memory/memory_resource_manager.h"
#include "subsys/device_tree/dt_display.h"
#include "domain/ui_domain/lvgl_lock.h"

#include "ui_renderer_service.h"

namespace eerie_leap::domain::ui_domain::services {

using namespace eerie_leap::subsys::device_tree;
using eerie_leap::utilities::memory::Mrm;

LOG_MODULE_REGISTER(renderer_logger);

#if CONFIG_EERIE_LEAP_UI_ROTATION != 0

// The flush thread signals completion through the callback this replaces, so
// leaving it enabled would park the renderer on a semaphore nobody gives.
BUILD_ASSERT(!IS_ENABLED(CONFIG_LV_Z_FLUSH_THREAD),
    "CONFIG_LV_Z_FLUSH_THREAD must be disabled when the UI is rotated");

namespace {

// The scanout only adopts an external frame whose address is cache line aligned.
constexpr size_t frame_alignment_ = 64;

struct FrameDeleter {
    size_t size = 0;

    void operator()(uint8_t* frame) const {
        if(frame != nullptr)
            Mrm::GetExtPmr()->deallocate(frame, size, frame_alignment_);
    }
};

// Holds the whole rotated frame between refreshes: LVGL only hands over the
// areas it redrew, and the scanout reads all of it. A second frame to alternate
// with only pays off once a refresh rewrites all of it, so until then the
// untouched parts of the frame off screen would be a refresh behind.
constexpr size_t frame_count_ = 1;
std::unique_ptr<uint8_t[], FrameDeleter> frames[frame_count_];
size_t draw_frame_ = 0;

// A cache line holds 32 RGB565 pixels, so a 32x32 tile is the square that keeps
// both sides of the transpose on lines it uses completely.
constexpr int32_t rotation_tile_ = 32;

// lv_draw_sw_rotate() transposes a column at a time, which steps the source a
// whole row per pixel and so spends a cache line fetch on every one of them.
// Tiling on the destination row (x) keeps a band of whole destination rows and
// the source columns feeding them resident together: 32 * (720*2 + 1280*2) is
// 92 KB, inside the 128 KB L2, so each line is filled once and written back once.
void RotateTiledRgb565(const uint16_t* source, uint16_t* destination, int32_t width,
    int32_t height, int32_t source_stride, int32_t destination_stride, bool clockwise) {

    for(int32_t tile_x = 0; tile_x < width; tile_x += rotation_tile_) {
        const int32_t tile_width = MIN(tile_x + rotation_tile_, width);

        for(int32_t tile_y = 0; tile_y < height; tile_y += rotation_tile_) {
            const int32_t tile_height = MIN(tile_y + rotation_tile_, height);

            for(int32_t x = tile_x; x < tile_width; ++x) {
                const uint16_t* row = source + (size_t)tile_y * source_stride + x;

                uint16_t* column = clockwise
                    ? destination + (size_t)(width - x - 1) * destination_stride + tile_y
                    : destination + (size_t)x * destination_stride + (height - tile_y - 1);
                const int32_t step = clockwise ? 1 : -1;

                for(int32_t y = tile_y; y < tile_height; ++y) {
                    *column = *row;
                    column += step;
                    row += source_stride;
                }
            }
        }
    }
}

void Rotate(const uint8_t* source, uint8_t* destination, int32_t width, int32_t height,
    int32_t source_stride, int32_t destination_stride, lv_display_rotation_t rotation,
    lv_color_format_t color_format) {

    const bool quarter_turn = rotation == LV_DISPLAY_ROTATION_90
        || rotation == LV_DISPLAY_ROTATION_270;

    if(!quarter_turn || color_format != LV_COLOR_FORMAT_RGB565) {
        lv_draw_sw_rotate(source, destination, width, height, source_stride,
            destination_stride, rotation, color_format);
        return;
    }

    RotateTiledRgb565(reinterpret_cast<const uint16_t*>(source),
        reinterpret_cast<uint16_t*>(destination), width, height,
        source_stride / (int32_t)sizeof(uint16_t),
        destination_stride / (int32_t)sizeof(uint16_t),
        rotation == LV_DISPLAY_ROTATION_90);
}

#ifdef CONFIG_ESP32_PPA

ppa_client_handle_t ppa_client = nullptr;

// LVGL counts its rotation the way the PPA counts its own, counter-clockwise.
ppa_srm_rotation_angle_t ToPpaRotation(lv_display_rotation_t rotation) {
    switch(rotation) {
        case LV_DISPLAY_ROTATION_90: return PPA_SRM_ROTATION_ANGLE_90;
        case LV_DISPLAY_ROTATION_180: return PPA_SRM_ROTATION_ANGLE_180;
        case LV_DISPLAY_ROTATION_270: return PPA_SRM_ROTATION_ANGLE_270;
        default: return PPA_SRM_ROTATION_ANGLE_0;
    }
}

bool RotateAccelerated(uint8_t* source, int32_t width, int32_t height, uint32_t source_stride,
    int32_t destination_x, int32_t destination_y, int32_t panel_width, int32_t panel_height,
    uint32_t panel_stride, lv_display_rotation_t rotation, lv_color_format_t color_format) {

    if(ppa_client == nullptr || color_format != LV_COLOR_FORMAT_RGB565)
        return false;

    // The engine reads the area over 2D-DMA and the driver only syncs its own
    // descriptors, so what LVGL just drew has to leave the cache first.
    sys_cache_data_flush_range(source, source_stride * (size_t)height);

    ppa_srm_oper_config_t config {};

    config.in.buffer = source;
    config.in.pic_w = source_stride / LV_COLOR_FORMAT_GET_SIZE(color_format);
    config.in.pic_h = height;
    config.in.block_w = width;
    config.in.block_h = height;
    config.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    config.out.buffer = frames[draw_frame_].get();
    config.out.buffer_size = panel_stride * (uint32_t)panel_height;
    config.out.pic_w = panel_width;
    config.out.pic_h = panel_height;
    config.out.block_offset_x = destination_x;
    config.out.block_offset_y = destination_y;
    config.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    config.rotation_angle = ToPpaRotation(rotation);
    config.scale_x = 1.0f;
    config.scale_y = 1.0f;
    config.mode = PPA_TRANS_MODE_BLOCKING;

    const esp_err_t status = ppa_do_scale_rotate_mirror(ppa_client, &config);

    if(status != ESP_OK) {
        // A rejected area is rotated by the CPU into a frame the engine writes
        // around, and the two disagree wherever they share a cache line.
        static bool reported;

        if(!reported) {
            reported = true;
            LOG_WRN("PPA rejected a %dx%d area at %d,%d (%d), rotating it on the CPU.",
                width, height, destination_x, destination_y, status);
        }

        return false;
    }

    return true;
}

#else

bool RotateAccelerated(uint8_t*, int32_t, int32_t, uint32_t, int32_t, int32_t, int32_t,
    int32_t, uint32_t, lv_display_rotation_t, lv_color_format_t) {

    return false;
}

#endif

lv_display_rotation_t ToLvglRotation(int degrees) {
    switch(degrees) {
        case 90: return LV_DISPLAY_ROTATION_90;
        case 180: return LV_DISPLAY_ROTATION_180;
        case 270: return LV_DISPLAY_ROTATION_270;
        default: return LV_DISPLAY_ROTATION_0;
    }
}

void RotatedFlushCb(lv_display_t* display, const lv_area_t* area, uint8_t* px_map) {
    const lv_color_format_t color_format = lv_display_get_color_format(display);
    const uint8_t pixel_size = LV_COLOR_FORMAT_GET_SIZE(color_format);
    const int32_t panel_width = lv_display_get_original_horizontal_resolution(display);
    const int32_t panel_height = lv_display_get_original_vertical_resolution(display);
    const uint32_t panel_stride = lv_draw_buf_width_to_stride(panel_width, color_format);

    const int32_t source_width = lv_area_get_width(area);
    const int32_t source_height = lv_area_get_height(area);
    const uint32_t source_stride = lv_draw_buf_width_to_stride(source_width, color_format);

    lv_area_t rotated = *area;
    lv_display_rotate_area(display, &rotated);

    // Rotating into the frame at its final position is the whole copy: the
    // scanout is pointed at this buffer, so nothing moves it again afterwards.
    uint8_t* destination = frames[draw_frame_].get()
        + static_cast<size_t>(rotated.y1) * panel_stride
        + static_cast<size_t>(rotated.x1) * pixel_size;

    const lv_display_rotation_t rotation = lv_display_get_rotation(display);

    if(!RotateAccelerated(px_map, source_width, source_height, source_stride, rotated.x1,
           rotated.y1, panel_width, panel_height, panel_stride, rotation, color_format)) {

        Rotate(px_map, destination, source_width, source_height, source_stride,
            panel_stride, rotation, color_format);
    }

    if(!lv_display_flush_is_last(display)) {
        lv_display_flush_ready(display);
        return;
    }

    display_buffer_descriptor descriptor {};
    descriptor.buf_size = panel_stride * static_cast<uint32_t>(panel_height);
    descriptor.width = static_cast<uint16_t>(panel_width);
    descriptor.height = static_cast<uint16_t>(panel_height);
    descriptor.pitch = static_cast<uint16_t>(panel_width);

    display_write(DtDisplay::Get(), 0, 0, &descriptor, frames[draw_frame_].get());

    // display_write() only returns once the frame this one replaces has left
    // the screen, so the one swapped in here is free to be drawn into.
    draw_frame_ = (draw_frame_ + 1) % frame_count_;

    lv_display_flush_ready(display);
}

int InitializeRotation(lv_display_t* display) {
    const lv_color_format_t color_format = lv_display_get_color_format(display);
    const int32_t width = lv_display_get_original_horizontal_resolution(display);
    const int32_t height = lv_display_get_original_vertical_resolution(display);
    const size_t size =
        static_cast<size_t>(lv_draw_buf_width_to_stride(width, color_format)) * height;

    for(size_t i = 0; i < frame_count_; ++i) {
        auto* frame = static_cast<uint8_t*>(Mrm::GetExtPmr()->allocate(size, frame_alignment_));
        if(frame == nullptr) {
            LOG_ERR("Failed to allocate the %zu byte rotated frame.", size);
            return -ENOMEM;
        }

        memset(frame, 0, size);
        frames[i] = std::unique_ptr<uint8_t[], FrameDeleter>(frame, FrameDeleter{size});

#ifdef CONFIG_ESP32_PPA
        // Both the scanout and the accelerator reach the frame over DMA, so the
        // zeroing has to be in memory before either of them looks at it.
        sys_cache_data_flush_range(frame, size);
#endif
    }

#ifdef CONFIG_ESP32_PPA
    ppa_client_config_t client_config {};
    client_config.oper_type = PPA_OPERATION_SRM;
    client_config.max_pending_trans_num = 1;
    client_config.data_burst_length = PPA_DATA_BURST_LENGTH_128;

    if(ppa_register_client(&client_config, &ppa_client) != ESP_OK) {
        ppa_client = nullptr;
        LOG_WRN("No PPA client, rotating on the CPU.");
    }
#endif

    lv_display_set_rotation(display, ToLvglRotation(CONFIG_EERIE_LEAP_UI_ROTATION));
    lv_display_set_flush_cb(display, RotatedFlushCb);

    LOG_INF("UI rotated by %d degrees: %dx%d.", CONFIG_EERIE_LEAP_UI_ROTATION,
        lv_display_get_horizontal_resolution(display),
        lv_display_get_vertical_resolution(display));

    return 0;
}

} // namespace

#endif

UiRendererService::UiRendererService() {
    // NOTE: Stack should be allocated on internal RAM to take an advantage of DMA for rendering
    thread_ = std::make_unique<Thread>(
        "ui_renderer_service",
        this,
        UiRendererService::k_stack_size_,
        UiRendererService::k_priority_,
        false);
}

UiRendererService::~UiRendererService() {
    Stop();
}

int UiRendererService::Initialize() {
    if(DtDisplay::Get() == nullptr) {
        LOG_ERR("Display not found, aborting");
        return -1;
    }

    if(!device_is_ready(DtDisplay::Get())) {
		LOG_ERR("Display not ready, aborting");
		return -1;
	}

    thread_->Initialize();

#if CONFIG_EERIE_LEAP_UI_ROTATION != 0
    auto* display = lv_display_get_default();
    if(display == nullptr) {
        LOG_ERR("LVGL display not found, aborting");
        return -1;
    }

    if(InitializeRotation(display) != 0)
        return -1;
#endif

    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), LV_PART_MAIN | LV_STATE_DEFAULT);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN | LV_STATE_DEFAULT);

    return 0;
}

void UiRendererService::Start() {
    running_ = true;
    thread_->Start();

    LOG_INF("UI renderer service started.");
}

void UiRendererService::Stop() {
    running_ = false;
    thread_->Join();
}

void UiRendererService::ThreadEntry() {
    while(running_)
        Render();
}

void UiRendererService::Render() {
    LvglLock::GetInstance().Lock();
    uint32_t sleep_ms = lv_timer_handler();
    LvglLock::GetInstance().Unlock();

    k_msleep(MIN(sleep_ms, INT32_MAX));
}

} // namespace eerie_leap::domain::ui_domain::services
