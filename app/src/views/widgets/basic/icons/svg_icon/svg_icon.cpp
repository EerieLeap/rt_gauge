#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string_view>

#include <lvgl_mem.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/mem_stats.h>

#include "domain/ui_domain/lvgl_lock.h"
#include "domain/ui_domain/models/widget_property.h"
#include "subsys/assets/asset_kind.h"
#include "subsys/assets/asset_provider.h"
#include "subsys/assets/asset_store.h"
#include "utilities/memory/memory_resource_manager.h"

#include <misc/cache/instance/lv_image_cache.h>
// LVGL's SVG renderer and tokenizer: private headers, without the C++ guard of the public ones.
extern "C" {
#include <image/svg/lv_svg_render.h>
#include <image/svg/lv_svg_token.h>
}

#include "svg_icon.h"

namespace eerie_leap::views::widgets::basic::icons {

using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;

using eerie_leap::domain::ui_domain::ScopedLvglLock;
// Inside eerie_leap::views a bare `assets::` is eerie_leap::views::assets, hence the declaration.
using eerie_leap::subsys::assets::AssetKind;

LOG_MODULE_REGISTER(svg_icon_logger);

namespace {

// The limit of the shape icons and the Lottie widget.
constexpr size_t MAX_PIXEL_BYTES = 4 * 1024 * 1024;
// LVGL builds and frees a document's tree recursively, and draws each nested group with a recursive call that keeps
// about 500 bytes on the stack (496 on the S3), on a thread that renders a screen with 6 to 8 KiB.
constexpr size_t MAX_DEPTH = 6;
// The LVGL heap a document takes while it draws, which peaks with the render list and the shapes queued from it:
// measured at 1.0 to 1.2 KiB an element and 2 to 4 bytes a byte of path data, these keep a margin of 1.7 or more.
constexpr size_t HEAP_PER_ELEMENT = 1536;
constexpr size_t HEAP_PER_DOCUMENT_BYTE = 8;

using Document = std::unique_ptr<lv_svg_node_t, decltype(&lv_svg_node_delete)>;
using RenderList = std::unique_ptr<lv_svg_render_obj_t, decltype(&lv_svg_render_delete)>;
using DrawDescriptor = std::unique_ptr<lv_draw_vector_dsc_t, decltype(&lv_draw_vector_dsc_delete)>;

// The elements of a document whose tree LVGL's parser builds within MAX_DEPTH, counted with the parser's own
// tokenizer before the tree exists; nullopt for a deeper one. Like the parser, only an end tag that matches the
// innermost open tag closes it, and nothing closes the root, so no end tag can hide a level.
std::optional<size_t> CountElements(std::string_view text) {
    struct Scan {
        std::array<std::string_view, MAX_DEPTH> open;
        size_t depth = 0;
        size_t elements = 0;
    } scan;

    const bool is_within_depth = _lv_svg_tokenizer(
        text.data(), text.size(),
        [](_lv_svg_token_t* token, void* user_data) {
            auto& scan = *static_cast<Scan*>(user_data);
            const std::string_view name(token->start, SVG_TOKEN_LEN(token));

            if(token->type == LV_SVG_TOKEN_END) {
                if(scan.depth > 1 && scan.open[scan.depth - 1] == name)
                    --scan.depth;
                return true;
            }
            if(token->type != LV_SVG_TOKEN_BEGIN)
                return true;

            ++scan.elements;
            if(token->flat)
                return true;
            if(scan.depth == MAX_DEPTH)
                return false;

            scan.open[scan.depth++] = name;
            return true;
        },
        &scan
    );

    return is_within_depth ? std::optional(scan.elements) : std::nullopt;
}

// Whether LVGL draws the parsed document safely. It follows a <use> without checking that it refers to anything or
// not to itself, calls a fill or stroke that refers to an element other than a gradient or a solid colour through a
// null pointer, and ThorVG generates any number of dashes, however short.
bool IsDrawable(const lv_svg_render_obj_t* list) {
    const auto is_paint = [list](const char* reference) {
        if(reference == nullptr)
            return true;

        // LVGL takes the first element with the name, and draws nothing for a name it does not find.
        for(const auto* obj = list; obj != nullptr; obj = obj->next) {
            if(obj->id != nullptr && std::strcmp(reference, obj->id) == 0)
                return obj->clz->set_paint_ref != nullptr;
        }
        return true;
    };

    for(const auto* obj = list; obj != nullptr; obj = obj->next) {
        if(obj->tag == LV_SVG_TAG_USE
            || lv_array_size(&obj->dsc.stroke_dsc.dash_pattern) > 0
            || !is_paint(obj->fill_ref)
            || !is_paint(obj->stroke_ref))
            return false;
    }
    return true;
}

// ThorVG draws into an ARGB8888 layer in premultiplied colour, which LVGL reads as straight alpha.
void Unpremultiply(const lv_draw_buf_t& buffer) {
    for(uint32_t y = 0; y < buffer.header.h; ++y) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the ARGB8888 buffer's bytes are LVGL pixels
        auto* row = reinterpret_cast<lv_color32_t*>(buffer.data + y * buffer.header.stride);
        for(auto& pixel : std::span(row, buffer.header.w)) {
            const uint32_t alpha = pixel.alpha;
            if(alpha == 0 || alpha == LV_OPA_COVER)
                continue;

            const auto unscale = [alpha](uint8_t channel) {
                return static_cast<uint8_t>(std::min<uint32_t>(255, (channel * 255U + alpha / 2) / alpha));
            };
            pixel.red = unscale(pixel.red);
            pixel.green = unscale(pixel.green);
            pixel.blue = unscale(pixel.blue);
        }
    }
}

} // namespace

SvgIcon::SvgIcon(std::shared_ptr<Frame> parent) :
    IconBase(std::move(parent)),
    pixels_(Mrm::GetExtPmr()) {}

SvgIcon::~SvgIcon() {
    ScopedLvglLock lvgl_guard;

    // The widget can still hold the shared Frame while this icon frees the pixels its image reads.
    if(image_object_ != nullptr) {
        lv_image_set_src(image_object_, nullptr);
        lv_image_cache_drop(&image_);
    }
}

void SvgIcon::RegisterProperties(WidgetPropertyStore& store) {
    IconBase::RegisterProperties(store);
    store.Register(WidgetPropertyType::FILE_PATH, ConfigValue{ std::pmr::string{} }, PropertyChangeEffect::Rebuild);
    store.Register(WidgetPropertyType::WIDTH_PX, ConfigValue{ 0 }, PropertyChangeEffect::Rebuild);
    store.Register(WidgetPropertyType::HEIGHT_PX, ConfigValue{ 0 }, PropertyChangeEffect::Rebuild);
}

int SvgIcon::ApplyTheme(const ITheme&) {
    return 0;
}

void SvgIcon::Configure(std::shared_ptr<WidgetPropertyStore> properties) {
    IconBase::Configure(std::move(properties));

    // Read for the next render: a document is drawn on a thread that renders screens, never on a bus worker.
    file_path_ = properties_->GetAs<std::pmr::string>(WidgetPropertyType::FILE_PATH, "");
    width_px_ = ReadPixels(*properties_, WidgetPropertyType::WIDTH_PX, 0, true);
    height_px_ = ReadPixels(*properties_, WidgetPropertyType::HEIGHT_PX, 0, true);
}

int SvgIcon::DoRender() {
    // Rasterize() replaces the pixels the object shows.
    if(image_object_ != nullptr) {
        lv_image_set_src(image_object_, nullptr);
        lv_image_cache_drop(&image_);
    }
    image_ = {};

    bool is_drawn = false;
    try {
        is_drawn = Rasterize();
    } catch(const std::bad_alloc&) {
        LOG_ERR("Not enough memory for SVG '%s'.", file_path_.c_str());
    }
    if(!is_drawn) {
        pixels_.clear();
        return -1;
    }

    if(image_object_ == nullptr) {
        image_object_ = lv_image_create(parent_->GetObject());
        container_ = std::make_shared<Frame>(Frame::Create(image_object_).Build());
        lv_image_set_antialias(image_object_, true);
        lv_obj_set_size(image_object_, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_align(image_object_, LV_ALIGN_CENTER);
    }
    lv_image_set_src(image_object_, &image_);

    return 0;
}

bool SvgIcon::Rasterize() {
    if(file_path_.empty() || width_px_ < 0 || height_px_ < 0 || context_.assets_manager == nullptr)
        return false;

    const auto provider = context_.assets_manager->Find(AssetKind::IMAGES);
    if(provider == nullptr || provider->store == nullptr)
        return false;

    // LVGL does not survive running out of its heap, so a document that may not fit is refused, by its size before
    // it is read and by its elements after. Without CONFIG_SYS_HEAP_RUNTIME_STATS the heap reports nothing free.
    sys_memory_stats heap{};
    lvgl_heap_stats(&heap);
    const auto size = provider->store->GetSize(file_path_);
    if(!size.has_value() || *size == 0 || *size > heap.free_bytes / HEAP_PER_DOCUMENT_BYTE) {
        LOG_ERR(
            "SVG '%s' is missing, empty or too large for the LVGL heap's %zu free bytes.", file_path_.c_str(),
            heap.free_bytes
        );
        return false;
    }

    // LVGL's tokenizer reads one byte past the text.
    auto source = provider->store->Load(file_path_);
    source.push_back(0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the asset's bytes are the document's text
    const std::string_view text(reinterpret_cast<const char*>(source.data()), source.size() - 1);
    const auto elements = text.empty() ? std::nullopt : CountElements(text);
    if(!elements.has_value()) {
        LOG_ERR("SVG '%s' cannot be read or nests deeper than %zu elements.", file_path_.c_str(), MAX_DEPTH);
        return false;
    }
    if(*elements * HEAP_PER_ELEMENT + text.size() * HEAP_PER_DOCUMENT_BYTE > heap.free_bytes) {
        LOG_ERR(
            "SVG '%s' has too many elements for the LVGL heap's %zu free bytes.", file_path_.c_str(), heap.free_bytes
        );
        return false;
    }

    Document document(lv_svg_load_data(text.data(), text.size()), lv_svg_node_delete);
    const RenderList list(lv_svg_render_create(document.get()), lv_svg_render_delete);
    document.reset();

    // The root's width and height, or else its viewBox, are the document's own size.
    float document_width = 0;
    float document_height = 0;
    if(list == nullptr
        || lv_svg_render_get_viewport_size(list.get(), &document_width, &document_height) != LV_RESULT_OK
        || !IsDrawable(list.get())) {
        LOG_ERR(
            "SVG '%s' is not an SVG document, or has <use>, dashes or a fill that LVGL cannot draw.", file_path_.c_str()
        );
        return false;
    }
    if(!(
           document_width > 0 && document_height > 0 && std::isfinite(document_width) && std::isfinite(document_height)
       )) {
        LOG_ERR("SVG '%s' has neither a size nor a viewBox.", file_path_.c_str());
        return false;
    }

    float scale = 1.0F;
    if(width_px_ > 0 && height_px_ > 0)
        scale = std::min(
            static_cast<float>(width_px_) / document_width, static_cast<float>(height_px_) / document_height
        );
    else if(width_px_ > 0)
        scale = static_cast<float>(width_px_) / document_width;
    else if(height_px_ > 0)
        scale = static_cast<float>(height_px_) / document_height;

    const float width = width_px_ > 0 ? static_cast<float>(width_px_) : std::round(document_width * scale);
    const float height = height_px_ > 0 ? static_cast<float>(height_px_) : std::round(document_height * scale);
    if(!(width >= 1 && height >= 1 && width <= MAX_DIMENSION && height <= MAX_DIMENSION))
        return false;

    const auto width_px = static_cast<uint32_t>(width);
    const auto height_px = static_cast<uint32_t>(height);
    const uint32_t stride = lv_draw_buf_width_to_stride(width_px, LV_COLOR_FORMAT_ARGB8888);
    const size_t bytes = static_cast<size_t>(stride) * height_px;
    // An image header keeps its stride in 16 bits.
    if(bytes > MAX_PIXEL_BYTES || stride > UINT16_MAX) {
        LOG_ERR("SVG '%s' at %ux%u exceeds the pixel limits.", file_path_.c_str(), width_px, height_px);
        return false;
    }

    pixels_.assign(bytes + LV_DRAW_BUF_ALIGN, 0);
    auto* data = static_cast<uint8_t*>(lv_draw_buf_align(pixels_.data(), LV_COLOR_FORMAT_ARGB8888));
    lv_draw_buf_t buffer{};
    if(lv_draw_buf_init(&buffer, width_px, height_px, LV_COLOR_FORMAT_ARGB8888, stride, data, bytes) != LV_RESULT_OK)
        return false;

    {
        // The canvas goes first: deleting it drops its image cache entry, which points at the buffer.
        auto canvas_frame = Frame::Create(lv_canvas_create(parent_->GetObject())).Build();
        auto* canvas = canvas_frame.GetObject();
        lv_obj_set_hidden(canvas, true);
        lv_canvas_set_draw_buf(canvas, &buffer);

        lv_layer_t layer;
        lv_canvas_init_layer(canvas, &layer);
        const DrawDescriptor descriptor(lv_draw_vector_dsc_create(&layer), lv_draw_vector_dsc_delete);
        if(descriptor == nullptr) {
            lv_canvas_finish_layer(canvas, &layer);
            return false;
        }

        // Fitted and centered, as SVG's default preserveAspectRatio places a viewBox in its viewport.
        lv_draw_vector_dsc_translate(
            descriptor.get(), (width - document_width * scale) / 2, (height - document_height * scale) / 2
        );
        lv_draw_vector_dsc_scale(descriptor.get(), scale, scale);
        lv_draw_svg_render(descriptor.get(), list.get());
        lv_draw_vector(descriptor.get());
        lv_canvas_finish_layer(canvas, &layer);
    }
    Unpremultiply(buffer);

    image_.header.magic = LV_IMAGE_HEADER_MAGIC;
    image_.header.cf = LV_COLOR_FORMAT_ARGB8888;
    image_.header.w = width_px;
    image_.header.h = height_px;
    image_.header.stride = stride;
    image_.data_size = bytes;
    image_.data = data;

    return true;
}

} // namespace eerie_leap::views::widgets::basic::icons
