#pragma once

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <string>
#include <vector>

#include <lvgl.h>

#include "views/widgets/widget_base.h"

namespace eerie_leap::views::widgets::basic {

// Plays the Lottie JSON asset named by FILE_PATH at WIDTH_PX x HEIGHT_PX, or at the widget's
// size when either is unset. Those properties take effect on the next render. Playback
// follows the processing state, so an animation on an inactive screen costs nothing.
class LottieWidget : public WidgetBase {
private:
    std::pmr::string file_path_;
    int width_px_ = 0;
    int height_px_ = 0;

    // Straight alpha ARGB8888: LVGL's L8 blender cannot read premultiplied pixels.
    std::pmr::vector<uint8_t> buffer_;
    lv_draw_buf_t draw_buf_ {};
    lv_obj_t* lv_lottie_ = nullptr;
    bool is_playing_ = false;

    lv_obj_t* Create();
    void SetPlaying(bool playing);

protected:
    int DoRender() override;
    void OnProcessingUpdated(bool enabled) override;
    void RegisterProperties(WidgetPropertyStore& store) const override;
    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override;

public:
    // The leaf rule, plus a FILE_PATH: without an animation nothing renders.
    static void ValidateChildren(const WidgetConfiguration& configuration,
        std::span<const WidgetConfiguration* const> children);

    LottieWidget(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context);
    ~LottieWidget() override;

    int ApplyTheme(const ITheme& theme) override;

    WidgetType GetType() const override { return WidgetType::BasicLottie; }
};

} // namespace eerie_leap::views::widgets::basic
