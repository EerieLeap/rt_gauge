#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <variant>

#include <zephyr/logging/log.h>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/memory/scoped_new_resource.h"
#include "domain/ui_domain/models/widget_property.h"

#include "lottie_widget.h"

namespace eerie_leap::views::widgets::basic {

using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;

LOG_MODULE_REGISTER(lottie_widget_logger);

namespace {

// Matches the shape icon mask limit; larger frames would not fit the smaller boards anyway.
constexpr size_t max_buffer_bytes = 4 * 1024 * 1024;
constexpr int32_t max_extent = INT16_MAX;

// lv_lottie's own frame callback, shared by every instance.
lv_anim_exec_xcb_t render_frame = nullptr;

// ThorVG allocates its model and renderer state with plain `new`; keep that off the libc arena.
void RenderFrameOnExtHeap(void* lottie, int32_t frame) {
    ScopedNewResource ext_heap(Mrm::GetExtPmr());
    render_frame(lottie, frame);
}

} // namespace

void LottieWidget::ValidateChildren(
    const WidgetConfiguration& configuration,
    std::span<const WidgetConfiguration* const> children
) {
    WidgetBase::ValidateChildren(configuration, children);

    const auto it = configuration.properties.find(WidgetPropertyType::FILE_PATH);
    const auto* file_path = it != configuration.properties.end()
        ? std::get_if<std::pmr::string>(&it->second)
        : nullptr;
    if(file_path == nullptr || file_path->empty())
        throw std::invalid_argument("FILE_PATH must name a Lottie asset.");
}

LottieWidget::LottieWidget(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context)
    : WidgetBase(id, std::move(parent), std::move(context)), buffer_(Mrm::GetExtPmr()) {}

LottieWidget::~LottieWidget() {
    DetachDispatch();
    // Deleting the canvas drops its image cache entry, which still points at draw_buf_.
    content_frame_->SetChild(nullptr);
}

int LottieWidget::DoRender() {
    content_frame_->SetChild(nullptr);
    lv_lottie_ = nullptr;
    is_playing_ = false;

    auto* lottie = Create();
    if(lottie == nullptr) {
        LOG_ERR("Widget %u failed to load Lottie animation '%s'.", id_, file_path_.c_str());
        return -1;
    }

    auto frame = std::make_shared<Frame>(Frame::Create(lottie).Build());
    content_frame_->SetChild(frame);
    transform_.SetTargetFrame(std::move(frame));

    return 0;
}

lv_obj_t* LottieWidget::Create() {
    if(file_path_.empty() || context_.assets_manager == nullptr)
        return nullptr;

    auto* content = content_frame_->GetObject();
    lv_obj_update_layout(content);
    const int32_t width = width_px_ > 0 ? width_px_ : lv_obj_get_content_width(content);
    const int32_t height = height_px_ > 0 ? height_px_ : lv_obj_get_content_height(content);
    if(width <= 0 || height <= 0 || width > max_extent || height > max_extent)
        return nullptr;

    const uint32_t stride = lv_draw_buf_width_to_stride(width, LV_COLOR_FORMAT_ARGB8888);
    const size_t size = static_cast<size_t>(stride) * height;
    if(size > max_buffer_bytes)
        return nullptr;

    const auto source = context_.assets_manager->Load(file_path_);
    if(source.empty()) {
        LOG_ERR("Lottie asset '%s' is missing or empty.", file_path_.c_str());
        return nullptr;
    }

    buffer_.assign(size + LV_DRAW_BUF_ALIGN, 0);
    auto* data = lv_draw_buf_align(buffer_.data(), LV_COLOR_FORMAT_ARGB8888);
    if(lv_draw_buf_init(&draw_buf_, width, height, LV_COLOR_FORMAT_ARGB8888, stride, data, size) != LV_RESULT_OK)
        return nullptr;

    ScopedNewResource ext_heap(Mrm::GetExtPmr());
    auto* lottie = lv_lottie_create(content);
    // Starting its animation fails on a full LVGL heap, and lv_lottie_set_src_data() does not check.
    auto* animation = lv_lottie_get_anim(lottie);
    if(animation != nullptr) {
        render_frame = animation->exec_cb;
        lv_anim_set_exec_cb(animation, RenderFrameOnExtHeap);
        lv_lottie_set_draw_buf(lottie, &draw_buf_);
        // ThorVG parses a copy and renders the first frame, so the asset can go once this returns.
        lv_lottie_set_src_data(lottie, source.data(), source.size());
    }

    // A zero duration means ThorVG could not parse the asset.
    if(animation == nullptr || lv_anim_get_time(animation) == 0) {
        lv_obj_delete(lottie);
        buffer_.clear();
        buffer_.shrink_to_fit();
        return nullptr;
    }

    lv_obj_center(lottie);

    // It starts playing; the processing state applied after rendering decides when it may.
    lv_lottie_ = lottie;
    is_playing_ = true;
    SetPlaying(false);

    return lottie;
}

void LottieWidget::SetPlaying(bool playing) {
    if(lv_lottie_ == nullptr || playing == is_playing_)
        return;

    auto* animation = lv_lottie_get_anim(lv_lottie_);
    if(playing)
        lv_anim_resume(animation);
    else
        lv_anim_pause(animation);

    is_playing_ = playing;
}

int LottieWidget::ApplyTheme(const ITheme&) {
    return 0;
}

void LottieWidget::OnProcessingUpdated(bool enabled) {
    SetPlaying(enabled);
}

void LottieWidget::RegisterProperties(WidgetPropertyStore& store) const {
    WidgetBase::RegisterProperties(store);

    store.Register(WidgetPropertyType::FILE_PATH, ConfigValue { std::pmr::string { } }, PropertyChangeEffect::Rebuild);
    store.Register(WidgetPropertyType::WIDTH_PX, ConfigValue { 0 }, PropertyChangeEffect::Rebuild);
    store.Register(WidgetPropertyType::HEIGHT_PX, ConfigValue { 0 }, PropertyChangeEffect::Rebuild);
}

void LottieWidget::OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) {
    switch(type) {
        case WidgetPropertyType::FILE_PATH:
            file_path_ = ConfigValueAs<std::pmr::string>(value, "");
            break;

        case WidgetPropertyType::WIDTH_PX:
            width_px_ = ConfigValueAs<int>(value, 0);
            break;

        case WidgetPropertyType::HEIGHT_PX:
            height_px_ = ConfigValueAs<int>(value, 0);
            break;

        default:
            WidgetBase::OnPropertyChanged(type, value);
            break;
    }
}

} // namespace eerie_leap::views::widgets::basic
