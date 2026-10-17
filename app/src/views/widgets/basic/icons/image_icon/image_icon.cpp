#include <cstdint>

#include <zephyr/logging/log.h>

#include "utilities/memory/memory_resource_manager.h"
#include "subsys/assets/asset_kind.h"
#include "subsys/assets/asset_provider.h"
#include "subsys/assets/asset_store.h"
#include "domain/ui_domain/lvgl_lock.h"
#include "domain/ui_domain/models/widget_property.h"
#include "views/themes/theme_manager.h"

#include <misc/cache/instance/lv_image_cache.h>

#include "image_icon.h"

namespace eerie_leap::views::widgets::basic::icons {

using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::themes;

using eerie_leap::domain::ui_domain::ScopedLvglLock;
// Inside eerie_leap::views a bare `assets::` is eerie_leap::views::assets, hence the declaration.
using eerie_leap::subsys::assets::AssetKind;

LOG_MODULE_REGISTER(image_icon_logger);

namespace {

// The bytes LVGL reads for a width x height image in the native format with alpha. RGB565A8 keeps its
// alpha in a plane of its own, which lv_color_format_get_size() leaves out.
uint64_t GetImageBytes(int width, int height) {
    const uint64_t bytes_per_pixel = lv_color_format_get_size(LV_COLOR_FORMAT_NATIVE_WITH_ALPHA)
        + (LV_COLOR_FORMAT_NATIVE_WITH_ALPHA == LV_COLOR_FORMAT_RGB565A8 ? 1 : 0);

    return static_cast<uint64_t>(width) * height * bytes_per_pixel;
}

} // namespace

ImageIcon::ImageIcon(std::shared_ptr<Frame> parent)
    : IconBase(std::move(parent)) {}

ImageIcon::~ImageIcon() {
    ScopedLvglLock lvgl_guard;

    // The widget can still hold the shared Frame while this icon frees the descriptor and pixels its image
    // reads, so the object lets go of them first.
    if(lv_image_ != nullptr) {
        lv_image_set_src(lv_image_, nullptr);
        lv_image_cache_drop(&lv_image_descriptor_);
    }
}

void ImageIcon::RegisterProperties(WidgetPropertyStore& store) {
    IconBase::RegisterProperties(store);
    store.Register(WidgetPropertyType::FILE_PATH, ConfigValue { std::pmr::string { } }, PropertyChangeEffect::Rebuild);
    store.Register(WidgetPropertyType::IMG_WIDTH, ConfigValue { 0 }, PropertyChangeEffect::Rebuild);
    store.Register(WidgetPropertyType::IMG_HEIGHT, ConfigValue { 0 }, PropertyChangeEffect::Rebuild);
}

int ImageIcon::ApplyTheme(const ITheme&) {
    return 0;
}

int ImageIcon::DoRender() {
    lv_obj_t* image = Create(parent_->GetObject());
    if(image == nullptr)
        return -1;

    container_ = std::make_shared<Frame>(Frame::Create(image).Build());
    lv_image_ = image;

    return 0;
}

lv_obj_t* ImageIcon::Create(lv_obj_t* parent) {
    if(file_path_.empty())
        return nullptr;

    if(image_width_ <= 0 || image_height_ <= 0)
        return nullptr;

    if(ui_assets_manager_ == nullptr)
        return nullptr;

    const auto provider = ui_assets_manager_->Find(AssetKind::IMAGES);
    if(provider == nullptr || provider->store == nullptr)
        return nullptr;

    auto image_data = provider->store->Load(file_path_);
    if(image_data.empty())
        return nullptr;

    // A client can upload any file under an image's name, and LVGL reads width x height pixels from it.
    // Refused before it replaces image_data_, which a rendered image may still be showing.
    if(image_data.size() < GetImageBytes(image_width_, image_height_)) {
        LOG_ERR("Image '%s' holds %zu bytes, too few for %dx%d pixels.",
            file_path_.c_str(), image_data.size(), image_width_, image_height_);
        return nullptr;
    }

    image_data_ = std::pmr::vector<uint8_t>(image_data.get_allocator());
    image_data_ = std::move(image_data);

    // NOTE: Color format docs (.header.cf)
    // https://docs.lvgl.io/master/main-modules/images/color_formats.html
    // image data must follow CONFIG_LV_COLOR_DEPTH_... color format
    lv_image_descriptor_ = {
        .header = {
            .magic = LV_IMAGE_HEADER_MAGIC,
            .cf = LV_COLOR_FORMAT_NATIVE_WITH_ALPHA,
            .flags = 0,
            .w = image_width_,
            .h = image_height_,
        },
        .data_size = image_data_.value().size(),
        .data = image_data_.value().data()
    };

    auto lv_image = lv_image_create(parent_->GetObject());
    lv_image_set_src(lv_image, &lv_image_descriptor_);
    lv_image_set_antialias(lv_image, true);

    lv_obj_set_width(lv_image, LV_SIZE_CONTENT);
    lv_obj_set_height(lv_image, LV_SIZE_CONTENT);
    lv_obj_set_align(lv_image, LV_ALIGN_CENTER);

    return lv_image;
}

void ImageIcon::Configure(std::shared_ptr<WidgetPropertyStore> properties) {
    IconBase::Configure(std::move(properties));

    file_path_ = properties_->GetAs<std::pmr::string>(WidgetPropertyType::FILE_PATH, "");
    image_width_ = properties_->GetAs<int>(WidgetPropertyType::IMG_WIDTH, 0);
    image_height_ = properties_->GetAs<int>(WidgetPropertyType::IMG_HEIGHT, 0);

    if(!IsReady() || image_width_ <= 0 || image_height_ <= 0
        || image_width_ > UINT16_MAX || image_height_ > UINT16_MAX
        || (lv_image_descriptor_.header.w == image_width_ && lv_image_descriptor_.header.h == image_height_))
        return;

    // The rule Create() applies: the loaded file has to cover the new dimensions, alpha plane included.
    if(!image_data_.has_value() || GetImageBytes(image_width_, image_height_) > image_data_->size())
        return;

    lv_image_cache_drop(&lv_image_descriptor_);
    lv_image_descriptor_.header.w = image_width_;
    lv_image_descriptor_.header.h = image_height_;
    lv_image_descriptor_.header.stride = 0;
    lv_image_set_src(container_->GetObject(), &lv_image_descriptor_);
}

} // namespace eerie_leap::views::widgets::basic::icons
