#include <algorithm>
#include <cmath>
#include <memory>
#include <new>

#include <zephyr/logging/log.h>
#include <misc/cache/instance/lv_image_cache.h>

#include "utilities/memory/memory_resource_manager.h"
#include "views/themes/theme_manager.h"

#include "shape_icon_base.h"

namespace eerie_leap::views::widgets::basic::icons {

using namespace eerie_leap::domain::ui_domain::models;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::views::themes::ThemeManager;

LOG_MODULE_REGISTER(shape_icon_logger);

namespace {

// Keep malformed configuration from becoming LVGL's encoded percentage sizes or
// an unbounded allocation. Masks use external RAM where the board provides it.
constexpr int max_dimension = 32767;
constexpr size_t max_mask_bytes = 4 * 1024 * 1024;
constexpr int tile_size = 32;
constexpr int tile_padding = 2;
constexpr int tile_extent = tile_size + 2 * tile_padding;

using Path = std::unique_ptr<lv_vector_path_t, decltype(&lv_vector_path_delete)>;
using DrawBuffer = std::unique_ptr<lv_draw_buf_t, decltype(&lv_draw_buf_destroy)>;
using DrawDescriptor = std::unique_ptr<lv_draw_vector_dsc_t, decltype(&lv_draw_vector_dsc_delete)>;

} // namespace

int ShapeIconBase::ReadPixels(const WidgetPropertyStore& store, WidgetPropertyType type, int fallback, bool is_dimension) {
    double value = store.GetAs<double>(type, fallback);
    if(!std::isfinite(value) || (is_dimension && (value < 0 || value > max_dimension)))
        return is_dimension ? -1 : 0;

    return static_cast<int>(std::clamp(value, 0.0, static_cast<double>(max_dimension)));
}

ShapeIconBase::ShapeIconBase(std::shared_ptr<Frame> parent)
    : IconBase(std::move(parent)), mask_(Mrm::GetExtPmr()) {}

ShapeIconBase::~ShapeIconBase() {
    // The widget can still hold the shared Frame while this icon releases its pixels.
    if(image_object_ != nullptr)
        lv_image_set_src(image_object_, nullptr);
    lv_image_cache_drop(&image_);
}

void ShapeIconBase::RegisterProperties(WidgetPropertyStore& store) {
    IconBase::RegisterProperties(store);
    store.Register(WidgetPropertyType::WIDTH_PX, ConfigValue { 32 }, PropertyChangeEffect::Relayout);
    store.Register(WidgetPropertyType::HEIGHT_PX, ConfigValue { 32 }, PropertyChangeEffect::Relayout);
    store.Register(WidgetPropertyType::STROKE_PX, ConfigValue { 2 }, PropertyChangeEffect::Relayout);
}

bool ShapeIconBase::ReadGeometry() {
    bool changed = UpdateValue(width_px_, ReadPixels(*properties_, WidgetPropertyType::WIDTH_PX, 32, true));
    changed |= UpdateValue(height_px_, ReadPixels(*properties_, WidgetPropertyType::HEIGHT_PX, 32, true));
    changed |= UpdateValue(stroke_px_, ReadPixels(*properties_, WidgetPropertyType::STROKE_PX, 2, false));
    return changed;
}

lv_point_t ShapeIconBase::GetBounds() const {
    return { width_px_, height_px_ };
}

void ShapeIconBase::Configure(std::shared_ptr<WidgetPropertyStore> properties) {
    IconBase::Configure(std::move(properties));
    bool changed = ReadGeometry();
    if(is_ready_) {
        if(changed)
            UpdateImage();
        ApplyTheme(ThemeManager::GetInstance().GetCurrentTheme());
    }
}

int ShapeIconBase::DoRender() {
    if(image_object_ == nullptr) {
        image_object_ = lv_image_create(parent_->GetObject());
        container_ = std::make_shared<Frame>(Frame::Create(image_object_).Build());
        lv_obj_remove_style_all(image_object_);
        lv_obj_remove_flag(image_object_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(image_object_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_align(image_object_, LV_ALIGN_CENTER);
        lv_image_set_antialias(image_object_, true);
    }
    UpdateImage();
    return 0;
}

int ShapeIconBase::ApplyTheme(const ITheme& theme) {
    auto color = theme.GetAccentColor();
    lv_obj_set_style_image_recolor(image_object_, color.ToLvColor(), 0);
    lv_obj_set_style_image_recolor_opa(image_object_, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(image_object_, is_active_ && image_.data != nullptr ? color.ToLvOpa() : LV_OPA_TRANSP, 0);
    container_->Invalidate();
    return 0;
}

void ShapeIconBase::UpdateImage() {
    lv_image_set_src(image_object_, nullptr);
    lv_image_cache_drop(&image_);
    image_ = {};
    mask_.clear();

    auto bounds = GetBounds();
    int width = bounds.x;
    int height = bounds.y;
    lv_obj_set_size(image_object_, std::max(1, width), std::max(1, height));
    lv_obj_set_style_transform_pivot_x(image_object_, std::max(1, width) / 2, 0);
    lv_obj_set_style_transform_pivot_y(image_object_, std::max(1, height) / 2, 0);

    if(width <= 0 || height <= 0 || !IsDrawable())
        return;
    if(static_cast<size_t>(lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_A8)) * height > max_mask_bytes) {
        LOG_WRN("Shape exceeds the 4 MiB mask limit.");
        return;
    }

    Path path(lv_vector_path_create(LV_VECTOR_PATH_QUALITY_HIGH), lv_vector_path_delete);

    try {
        float stroke_width = BuildPath(path.get(), width, height);
        Rasterize(path.get(), width, height, stroke_width);
        lv_image_set_src(image_object_, &image_);
    } catch(const std::bad_alloc&) {
        LOG_WRN("Not enough memory for shape icon.");
        mask_.clear();
        image_ = {};
    }
}

void ShapeIconBase::Rasterize(lv_vector_path_t* path, int width_px, int height_px, float stroke_px) {
    uint32_t stride = lv_draw_buf_width_to_stride(width_px, LV_COLOR_FORMAT_A8);
    mask_.resize(static_cast<size_t>(stride) * height_px, 0);

    // Rasterize into a fixed ARGB tile: the RGB565 vector backend otherwise needs
    // an intermediate buffer as large as the display. Only the alpha mask persists.
    // Discard a small halo around each tile so clipping at the rasterizer's edge
    // cannot leave isolated antialiasing pixels along tile boundaries.
    DrawBuffer tile(lv_draw_buf_create(tile_extent, tile_extent, LV_COLOR_FORMAT_ARGB8888, LV_STRIDE_AUTO), lv_draw_buf_destroy);
    if(tile == nullptr)
        throw std::bad_alloc();
    auto canvas_frame = Frame::Create(lv_canvas_create(parent_->GetObject())).Build();
    auto canvas = canvas_frame.GetObject();
    lv_obj_add_flag(canvas, LV_OBJ_FLAG_HIDDEN);
    lv_canvas_set_draw_buf(canvas, tile.get());

    for(int y = 0; y < height_px; y += tile_size) {
        for(int x = 0; x < width_px; x += tile_size) {
            lv_draw_buf_clear(tile.get(), nullptr);
            lv_layer_t layer;
            lv_canvas_init_layer(canvas, &layer);
            DrawDescriptor descriptor(lv_draw_vector_dsc_create(&layer), lv_draw_vector_dsc_delete);
            if(descriptor == nullptr) {
                lv_canvas_finish_layer(canvas, &layer);
                throw std::bad_alloc();
            }
            lv_draw_vector_dsc_translate(descriptor.get(), tile_padding - x, tile_padding - y);
            lv_draw_vector_dsc_set_fill_color(descriptor.get(), lv_color_white());
            lv_draw_vector_dsc_set_fill_rule(descriptor.get(), LV_VECTOR_FILL_EVENODD);
            lv_draw_vector_dsc_set_fill_opa(descriptor.get(), stroke_px > 0 ? LV_OPA_TRANSP : LV_OPA_COVER);
            lv_draw_vector_dsc_set_stroke_color(descriptor.get(), lv_color_white());
            lv_draw_vector_dsc_set_stroke_opa(descriptor.get(), stroke_px > 0 ? LV_OPA_COVER : LV_OPA_TRANSP);
            lv_draw_vector_dsc_set_stroke_width(descriptor.get(), stroke_px);
            lv_draw_vector_dsc_set_stroke_cap(descriptor.get(), LV_VECTOR_STROKE_CAP_BUTT);
            lv_draw_vector_dsc_add_path(descriptor.get(), path);
            lv_draw_vector(descriptor.get());
            lv_canvas_finish_layer(canvas, &layer);

            for(int row = 0; row < std::min(tile_size, height_px - y); ++row) {
                auto pixels = reinterpret_cast<const lv_color32_t*>(tile->data + (row + tile_padding) * tile->header.stride);
                for(int column = 0; column < std::min(tile_size, width_px - x); ++column)
                    mask_[(y + row) * stride + x + column] = pixels[column + tile_padding].alpha;
            }
        }
    }

    image_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_.header.cf = LV_COLOR_FORMAT_A8;
    image_.header.w = width_px;
    image_.header.h = height_px;
    image_.header.stride = stride;
    image_.data_size = mask_.size();
    image_.data = mask_.data();
}

} // namespace eerie_leap::views::widgets::basic::icons
