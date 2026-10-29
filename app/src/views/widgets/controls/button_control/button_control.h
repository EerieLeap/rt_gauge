#pragma once

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <utility>

#include <lvgl.h>

#include "domain/ui_domain/models/navigation_intent.h"

#include "views/utilities/lvgl_font.h"
#include "views/widgets/controls/control_base.h"

namespace eerie_leap::views::widgets::controls {

using eerie_leap::domain::ui_domain::models::NavigationIntent;

// A button navigates on a click, reports its press and shows a value.
// - Click: runs its NAVIGATION_INTENT, bound or not; a target without an intent navigates nowhere.
// - Press: with an outbound binding on VALUE it publishes 1 when LVGL marks it pressed and 0 when it is released,
//   loses the press, or a suspension or a teardown ends it, each change once, as a physical button's contact does.
//   It never writes VALUE itself, and it owns its touch area as a slider owns its drag, so no swipe starts on it.
// - Shown: VALUE, which an inbound binding writes, lights the button (LV_STATE_CHECKED, drawn with the pair a press
//   uses) while it is not 0, so one InOut binding presses and shows the reply.
class ButtonControl : public ControlBase {
private:
    lv_obj_t* lv_button_ = nullptr;
    lv_obj_t* lv_label_ = nullptr;
    // The configured font; the theme's when empty.
    std::optional<eerie_leap::views::utilities::LvglFont> font_;

    std::pmr::string label_;
    NavigationIntent intent_ = NavigationIntent::None;

    // What it identifies is the intent's business: a screen group, or a screen.
    std::optional<uint32_t> target_id_;

    // Whether the last press state sent was a press, so each change goes out once.
    bool is_press_published_ = false;

    // Resolves what a click should ask navigation to do, or nothing if the button
    // is not configured to navigate.
    std::optional<std::pair<NavigationIntent, uint32_t>> ResolveNavigation() const;
    void Navigate();

    void PublishPress(bool is_pressed);
    void UpdateChecked();

    int DoRender() override;
    int ApplyTheme(const ITheme& theme) override;

protected:
    void OnControlEvent(lv_event_code_t code) override;
    void OnProcessingSuspended() override;
    void OnConfigured() override;

public:
    explicit ButtonControl(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context);
    ~ButtonControl() override;
    ButtonControl(const ButtonControl&) = delete;
    ButtonControl& operator=(const ButtonControl&) = delete;
    ButtonControl(ButtonControl&&) = delete;
    ButtonControl& operator=(ButtonControl&&) = delete;

protected:
    void RegisterProperties(WidgetPropertyStore& store) const override;
    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override;

public:
    WidgetType GetType() const override {
        return WidgetType::ControlButton;
    }
};

} // namespace eerie_leap::views::widgets::controls
