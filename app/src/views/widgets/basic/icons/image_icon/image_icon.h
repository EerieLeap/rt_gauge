#pragma once

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <vector>

#include <lvgl.h>

#include "views/utilities/image_cache.h"
#include "views/widgets/basic/icons/icon_base.h"

namespace eerie_leap::views::widgets::basic::icons {

class ImageIcon : public IconBase {
private:
    // What FILE_PATH names, told by its extension.
    enum class FileKind : uint8_t {
        None,
        // A raw `.bin` of pixels in the display's format, whose size IMG_WIDTH and IMG_HEIGHT give.
        RawPixels,
        // A `.lvimg`, which carries its own size and format.
        ImageFile,
        // Anything else, such as an SVG document: it draws nothing.
        Unsupported,
    };

    std::pmr::string file_path_;
    FileKind file_kind_ = FileKind::None;
    int image_width_ = 0;
    int image_height_ = 0;

    // A raw file's copy, private to this icon: the older form, which the app converts, keeps its own descriptor.
    lv_image_dsc_t lv_image_descriptor_{};
    std::optional<std::pmr::vector<uint8_t>> image_data_;
    // An image file's pixels, shared with every icon that shows the file.
    std::shared_ptr<const eerie_leap::views::utilities::LoadedImage> image_;
    // The object that reads the descriptor of image_ or lv_image_descriptor_ and the pixels behind it; null until
    // rendered.
    lv_obj_t* lv_image_ = nullptr;

    lv_obj_t* Create(lv_obj_t* parent);
    lv_obj_t* CreateFromRawPixels(lv_obj_t* parent);
    lv_obj_t* CreateFromImageFile(lv_obj_t* parent);

public:
    explicit ImageIcon(std::shared_ptr<Frame> parent);
    ~ImageIcon() override;
    ImageIcon(const ImageIcon&) = delete;
    ImageIcon& operator=(const ImageIcon&) = delete;
    ImageIcon(ImageIcon&&) = delete;
    ImageIcon& operator=(ImageIcon&&) = delete;

    static void RegisterProperties(WidgetPropertyStore& store);

    int ApplyTheme(const ITheme& theme) override;
    int DoRender() override;
    void Configure(std::shared_ptr<WidgetPropertyStore> properties) override;

    [[nodiscard]] IconType GetIconType() const override {
        return IconType::Image;
    }
};

} // namespace eerie_leap::views::widgets::basic::icons
