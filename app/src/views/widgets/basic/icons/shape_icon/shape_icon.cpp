#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <new>

#include <zephyr/logging/log.h>
#include <misc/cache/instance/lv_image_cache.h>

#include "utilities/memory/memory_resource_manager.h"
#include "views/themes/theme_manager.h"

#include "shape_icon.h"

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
using Triangle = std::array<lv_fpoint_t, 3>;

int ReadPixels(const WidgetPropertyStore& store, WidgetPropertyType type, int fallback, bool is_dimension) {
    double value = store.GetAs<double>(type, fallback);
    if(!std::isfinite(value) || (is_dimension && (value < 0 || value > max_dimension)))
        return is_dimension ? -1 : 0;

    return static_cast<int>(std::clamp(value, 0.0, static_cast<double>(max_dimension)));
}

float Distance(lv_fpoint_t a, lv_fpoint_t b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

lv_fpoint_t Incenter(const Triangle& vertices) {
    float a = Distance(vertices[1], vertices[2]);
    float b = Distance(vertices[2], vertices[0]);
    float c = Distance(vertices[0], vertices[1]);
    return {
        (a * vertices[0].x + b * vertices[1].x + c * vertices[2].x) / (a + b + c),
        (a * vertices[0].y + b * vertices[1].y + c * vertices[2].y) / (a + b + c)
    };
}

void AppendTriangle(lv_vector_path_t* path, const Triangle& vertices, float radius) {
    for(size_t i = 0; i < vertices.size(); ++i) {
        auto vertex = vertices[i];
        if(radius <= 0) {
            if(i == 0)
                lv_vector_path_move_to(path, &vertex);
            else
                lv_vector_path_line_to(path, &vertex);
            continue;
        }

        auto previous = vertices[(i + 2) % 3];
        auto next = vertices[(i + 1) % 3];
        float previous_length = Distance(vertex, previous);
        float next_length = Distance(vertex, next);
        lv_fpoint_t u { (previous.x - vertex.x) / previous_length, (previous.y - vertex.y) / previous_length };
        lv_fpoint_t v { (next.x - vertex.x) / next_length, (next.y - vertex.y) / next_length };
        // atan2 remains stable for very narrow triangles, where acos(dot) loses precision.
        float angle = std::atan2(std::abs(u.x * v.y - u.y * v.x), u.x * v.x + u.y * v.y);
        float tangent_length = radius / std::tan(angle / 2);
        lv_fpoint_t start { vertex.x + u.x * tangent_length, vertex.y + u.y * tangent_length };
        lv_fpoint_t end { vertex.x + v.x * tangent_length, vertex.y + v.y * tangent_length };

        if(i == 0)
            lv_vector_path_move_to(path, &start);
        else
            lv_vector_path_line_to(path, &start);
        lv_vector_path_arc_to(path, radius, radius, 0, false, true, &end);
    }
    lv_vector_path_close(path);
}

void AppendTriangleOutline(lv_vector_path_t* path, Triangle vertices, float radius, float thickness) {
    auto center = Incenter(vertices);
    // Both supported triangles have their base at the bottom of the bounding box.
    float inradius = vertices[1].y - center.y;
    radius = std::min(radius, inradius);
    AppendTriangle(path, vertices, radius);

    if(thickness <= 0 || thickness >= inradius)
        return;

    // Insetting all three edges scales a triangle about its incenter. The inner
    // contour is a hole (even-odd fill), so outlines preserve the background.
    float scale = (inradius - thickness) / inradius;
    for(auto& vertex : vertices) {
        vertex.x = center.x + (vertex.x - center.x) * scale;
        vertex.y = center.y + (vertex.y - center.y) * scale;
    }
    AppendTriangle(path, vertices, std::max(0.0F, radius - thickness));
}

} // namespace

ShapeIcon::ShapeIcon(std::shared_ptr<Frame> parent, IconType type)
    : IconBase(std::move(parent)), type_(type), mask_(Mrm::GetExtPmr()) {}

ShapeIcon::~ShapeIcon() {
    // The widget can still hold the shared Frame while this icon releases its pixels.
    if(image_object_ != nullptr)
        lv_image_set_src(image_object_, nullptr);
    lv_image_cache_drop(&image_);
}

void ShapeIcon::Configure(std::shared_ptr<WidgetPropertyStore> properties) {
    IconBase::Configure(std::move(properties));

    Settings next;
    next.width_px = ReadPixels(*properties_, WidgetPropertyType::WIDTH_PX, 32, true);
    next.height_px = ReadPixels(*properties_, WidgetPropertyType::HEIGHT_PX, 32, true);
    next.stroke_px = ReadPixels(*properties_, WidgetPropertyType::STROKE_PX, 2, false);
    next.corner_radius_px = ReadPixels(*properties_, WidgetPropertyType::CORNER_RAD_PX, 0, false);
    if(properties_->GetAs<double>(WidgetPropertyType::FILL_MODE, 0) == static_cast<int>(WidgetFillMode::Outline))
        next.fill_mode = WidgetFillMode::Outline;

    double direction = properties_->GetAs<double>(WidgetPropertyType::DIRECTION, 1);
    if(direction >= 1 && direction <= 4 && std::floor(direction) == direction)
        next.direction = static_cast<WidgetDirection>(static_cast<int>(direction));

    bool changed = next != settings_;
    settings_ = next;
    if(is_ready_) {
        if(changed)
            UpdateImage();
        ApplyTheme(ThemeManager::GetInstance().GetCurrentTheme());
    }
}

int ShapeIcon::DoRender() {
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

int ShapeIcon::ApplyTheme(const ITheme& theme) {
    auto color = theme.GetAccentColor();
    lv_obj_set_style_image_recolor(image_object_, color.ToLvColor(), 0);
    lv_obj_set_style_image_recolor_opa(image_object_, LV_OPA_COVER, 0);
    lv_obj_set_style_opa(image_object_, is_active_ && image_.data != nullptr ? color.ToLvOpa() : LV_OPA_TRANSP, 0);
    container_->Invalidate();
    return 0;
}

void ShapeIcon::UpdateImage() {
    lv_image_set_src(image_object_, nullptr);
    lv_image_cache_drop(&image_);
    image_ = {};
    mask_.clear();

    int width = settings_.width_px;
    int height = settings_.height_px;
    bool is_line = type_ == IconType::Line;
    bool horizontal = settings_.direction == WidgetDirection::LeftToRight
        || settings_.direction == WidgetDirection::RightToLeft;
    bool outline = settings_.fill_mode == WidgetFillMode::Outline;
    if(is_line && width >= 0 && height >= 0) {
        if(horizontal)
            height = std::max(height, settings_.stroke_px);
        else
            width = std::max(width, settings_.stroke_px);
    }

    lv_obj_set_size(image_object_, std::max(1, width), std::max(1, height));
    lv_obj_set_style_transform_pivot_x(image_object_, std::max(1, width) / 2, 0);
    lv_obj_set_style_transform_pivot_y(image_object_, std::max(1, height) / 2, 0);

    if(width <= 0 || height <= 0 || ((is_line || outline) && settings_.stroke_px <= 0))
        return;
    if(static_cast<size_t>(lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_A8)) * height > max_mask_bytes) {
        LOG_WRN("Shape exceeds the 4 MiB mask limit.");
        return;
    }

    Path path(lv_vector_path_create(LV_VECTOR_PATH_QUALITY_HIGH), lv_vector_path_delete);
    float w = width;
    float h = height;
    float thickness = settings_.stroke_px;
    float radius = std::min<float>(settings_.corner_radius_px, std::min(w, h) / 2);
    float stroke_width = 0;

    switch(type_) {
        case IconType::Rectangle:
            lv_vector_path_append_rectangle(path.get(), 0, 0, w, h, radius, radius);
            if(outline && settings_.stroke_px * 2 < std::min(w, h)) {
                float inner_radius = std::max(0.0F, radius - settings_.stroke_px);
                lv_vector_path_append_rectangle(path.get(), settings_.stroke_px, settings_.stroke_px,
                    w - 2 * settings_.stroke_px, h - 2 * settings_.stroke_px, inner_radius, inner_radius);
            }
            break;

        case IconType::TriangleIsosceles:
        case IconType::TriangleRight:
            AppendTriangleOutline(path.get(), Triangle {{
                { type_ == IconType::TriangleRight ? 0.0F : w / 2, 0 }, { w, h }, { 0, h }
            }}, settings_.corner_radius_px, outline ? settings_.stroke_px : 0);
            break;

        case IconType::Oval: {
            lv_fpoint_t center { w / 2, h / 2 };
            if(outline && settings_.stroke_px * 2 < std::min(w, h))
                stroke_width = settings_.stroke_px;
            lv_vector_path_append_circle(path.get(), &center, (w - stroke_width) / 2, (h - stroke_width) / 2);
            break;
        }

        case IconType::Line: {
            stroke_width = thickness;
            lv_fpoint_t start { horizontal ? 0.0F : w / 2, horizontal ? h / 2 : 0.0F };
            lv_fpoint_t end { horizontal ? w : w / 2, horizontal ? h / 2 : h };
            if(settings_.direction == WidgetDirection::RightToLeft || settings_.direction == WidgetDirection::BottomToTop)
                std::swap(start, end);
            lv_vector_path_move_to(path.get(), &start);
            lv_vector_path_line_to(path.get(), &end);
            break;
        }

        default:
            return;
    }

    try {
        Rasterize(path.get(), width, height, stroke_width);
        lv_image_set_src(image_object_, &image_);
    } catch(const std::bad_alloc&) {
        LOG_WRN("Not enough memory for shape icon.");
        mask_.clear();
        image_ = {};
    }
}

void ShapeIcon::Rasterize(lv_vector_path_t* path, int width_px, int height_px, float stroke_px) {
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
