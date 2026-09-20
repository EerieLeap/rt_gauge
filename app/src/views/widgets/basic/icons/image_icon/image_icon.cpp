#include "utilities/memory/memory_resource_manager.h"
#include "domain/ui_domain/models/widget_property.h"
#include "views/themes/theme_manager.h"

#include <misc/cache/instance/lv_image_cache.h>

#include "image_icon.h"

namespace eerie_leap::views::widgets::basic::icons {

using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::themes;

ImageIcon::ImageIcon(std::shared_ptr<Frame> parent)
    : IconBase(std::move(parent)) {}

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

    return 0;
}

lv_obj_t* ImageIcon::Create(lv_obj_t* parent) {
    if(file_path_.empty())
        return nullptr;

    if(image_width_ <= 0 || image_height_ <= 0)
        return nullptr;

    if(ui_assets_manager_ == nullptr)
        return nullptr;

    auto image_data = std::move(ui_assets_manager_->Load(file_path_));
    if(image_data.empty())
        return nullptr;
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

void ImageIcon::SetAnchorPoint(const lv_point_t& point) {
    IconBase::SetAnchorPoint(point);
    lv_image_set_pivot(container_->GetObject(), point.x, point.y);
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

    const uint64_t required_bytes = static_cast<uint64_t>(image_width_) * image_height_
        * lv_color_format_get_size(LV_COLOR_FORMAT_NATIVE_WITH_ALPHA);
    if(!image_data_.has_value() || required_bytes > image_data_->size())
        return;

    lv_image_cache_drop(&lv_image_descriptor_);
    lv_image_descriptor_.header.w = image_width_;
    lv_image_descriptor_.header.h = image_height_;
    lv_image_descriptor_.header.stride = 0;
    lv_image_set_src(container_->GetObject(), &lv_image_descriptor_);
}

} // namespace eerie_leap::views::widgets::basic::icons
