#pragma once

#include <memory_resource>
#include <vector>

#include "views/widgets/basic/icons/icon_base.h"

namespace eerie_leap::views::widgets::basic::icons {

// Owns the cached alpha mask and tiled rasterization shared by geometric icons.
// Concrete icons supply their geometry and declare the properties they consume.
class ShapeIconBase : public IconBase {
private:
    lv_obj_t* image_object_ = nullptr;
    lv_image_dsc_t image_ {};
    std::pmr::vector<uint8_t> mask_;

    void UpdateImage();
    void Rasterize(lv_vector_path_t* path, int width_px, int height_px, float stroke_px);

protected:
    int width_px_ = 32;
    int height_px_ = 32;
    int stroke_px_ = 2;

    static int ReadPixels(const WidgetPropertyStore& store, WidgetPropertyType type, int fallback, bool is_dimension);
    template<typename T>
    static bool UpdateValue(T& target, T value) {
        bool changed = target != value;
        target = value;
        return changed;
    }

    virtual bool ReadGeometry();
    virtual lv_point_t GetBounds() const;
    virtual bool IsDrawable() const = 0;
    // Returns the vector stroke width, or zero for a filled path.
    virtual float BuildPath(lv_vector_path_t* path, float width, float height) const = 0;

public:
    explicit ShapeIconBase(std::shared_ptr<Frame> parent);
    ~ShapeIconBase() override;

    static void RegisterProperties(WidgetPropertyStore& store);
    void Configure(std::shared_ptr<WidgetPropertyStore> properties) override;
    int DoRender() override;
    int ApplyTheme(const ITheme& theme) override;
};

} // namespace eerie_leap::views::widgets::basic::icons
