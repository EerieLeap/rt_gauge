#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <memory_resource>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/device.h>
#include <zephyr/drivers/display.h>
#include <lvgl_mem.h>

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

// LVGL starts a flush only once the previous one has completed, so a single
// scratch buffer serves all of them.
std::unique_ptr<std::pmr::vector<uint8_t>> rotation_buffer;

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

    const int32_t source_width = lv_area_get_width(area);
    const int32_t source_height = lv_area_get_height(area);
    const uint32_t source_stride = lv_draw_buf_width_to_stride(source_width, color_format);

    lv_area_t rotated = *area;
    lv_display_rotate_area(display, &rotated);

    const int32_t width = lv_area_get_width(&rotated);
    const int32_t height = lv_area_get_height(&rotated);
    const uint32_t stride = lv_draw_buf_width_to_stride(width, color_format);
    const size_t size = static_cast<size_t>(stride) * height;

    if(rotation_buffer == nullptr || size > rotation_buffer->size()) {
        LOG_ERR("Rotation buffer holds %zu bytes, this flush needs %zu.",
            rotation_buffer == nullptr ? 0U : rotation_buffer->size(), size);

        lv_display_flush_ready(display);
        return;
    }

    lv_draw_sw_rotate(px_map, rotation_buffer->data(), source_width, source_height,
        source_stride, stride, lv_display_get_rotation(display), color_format);

    display_buffer_descriptor descriptor {};
    descriptor.buf_size = size;
    descriptor.width = static_cast<uint16_t>(width);
    descriptor.height = static_cast<uint16_t>(height);
    descriptor.pitch = static_cast<uint16_t>(stride / pixel_size);
    // The DSI driver presents the framebuffer on the write that completes a frame.
    descriptor.frame_incomplete = !lv_display_flush_is_last(display);

    display_write(DtDisplay::Get(), rotated.x1, rotated.y1, &descriptor,
        rotation_buffer->data());

    lv_display_flush_ready(display);
}

int InitializeRotation(lv_display_t* display) {
    const lv_color_format_t color_format = lv_display_get_color_format(display);
    const int32_t width = lv_display_get_original_horizontal_resolution(display);
    const int32_t height = lv_display_get_original_vertical_resolution(display);

    // Rotating preserves the pixel count; the second term covers the row padding
    // a narrow rotated area picks up from the stride alignment.
    const size_t size = static_cast<size_t>(lv_draw_buf_width_to_stride(width, color_format))
            * height
        + static_cast<size_t>(LV_DRAW_BUF_STRIDE_ALIGN) * MAX(width, height);

    try {
        rotation_buffer = std::make_unique<std::pmr::vector<uint8_t>>(size, Mrm::GetExtPmr());
    } catch(const std::exception& ex) {
        LOG_ERR("Failed to allocate the %zu byte rotation buffer. %s", size, ex.what());
        return -ENOMEM;
    }

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
