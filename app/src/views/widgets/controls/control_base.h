#pragma once

#include <initializer_list>
#include <memory>

#include <lvgl.h>

#include "views/widgets/setting_widget_base.h"

namespace eerie_leap::views::widgets::controls {

using eerie_leap::views::widgets::SettingWidgetBase;

// Adds the interactive half to a setting-bound widget: LVGL event routing and,
// for the controls that own a drag, gesture capture.
class ControlBase : public SettingWidgetBase {
private:
    static void EventCb(lv_event_t* e);
    static void InputGuardCb(lv_event_t* event);

    lv_obj_t* event_object_ = nullptr;

protected:
    // Only a control that owns a drag may swallow the gesture; anything else
    // would carve a dead zone out of the screen's swipe navigation.
    ControlBase(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context, bool consumes_gestures);

    // Routes LVGL event codes to OnControlEvent(). A control drives exactly one
    // interactive object, so teardown is a single detach that cannot miss a code.
    void AttachEvents(lv_obj_t* object, std::initializer_list<lv_event_code_t> codes);

    virtual void OnControlEvent(lv_event_code_t code);
    void OnProcessingSuspended() override;

public:
    ~ControlBase() override;
};

} // namespace eerie_leap::views::widgets::controls
