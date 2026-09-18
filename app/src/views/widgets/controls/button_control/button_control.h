#pragma once

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <utility>

#include <lvgl.h>

#include "domain/ui_domain/models/navigation_intent.h"

#include "views/widgets/controls/control_base.h"

namespace eerie_leap::views::widgets::controls {

using eerie_leap::domain::ui_domain::models::NavigationIntent;

class ButtonControl : public ControlBase {
private:
    lv_obj_t* lv_button_ = nullptr;
    lv_obj_t* lv_label_ = nullptr;

    std::pmr::string label_;
    NavigationIntent intent_ = NavigationIntent::None;

    // What it identifies is the intent's business: a screen group, or a screen.
    std::optional<uint32_t> target_id_;

    // Resolves what a click should ask navigation to do, or nothing if the button
    // is not configured to navigate.
    std::optional<std::pair<NavigationIntent, uint32_t>> ResolveNavigation() const;

    int DoRender() override;
    int ApplyTheme(const ITheme& theme) override;

protected:
    void OnControlEvent(lv_event_code_t code) override;
    void OnProcessingSuspended() override;

public:
    explicit ButtonControl(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context);

protected:
    void RegisterProperties(WidgetPropertyStore& store) const override;
    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override;

public:
    WidgetType GetType() const override { return WidgetType::ControlButton; }
};

} // namespace eerie_leap::views::widgets::controls
