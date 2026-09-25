#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include <lvgl.h>

#include "domain/ui_domain/models/widget_configuration.h"
#include "views/utilitites/frame.h"
#include "views/widgets/widget_animation.h"
#include "views/widgets/widget_property_store.h"

namespace eerie_leap::views::widgets {

using domain::ui_domain::models::WidgetConfiguration;
using views::utilitites::Frame;

// Owns a presentation's shared anchors, retained rotation, and animation. The caller supplies
// readiness/eligibility callbacks and keeps their context alive until Detach. Frames are shared.
// Lifecycle calls run under the caller's LVGL lock; SetRotation, Detach, and LVGL
// callbacks acquire it themselves.
class WidgetTransform {
public:
    struct Callbacks {
        bool (*is_ready)(void* context);
        bool (*is_processing_eligible)(void* context);
    };

    WidgetTransform() = default;
    ~WidgetTransform();
    WidgetTransform(const WidgetTransform&) = delete;
    WidgetTransform& operator=(const WidgetTransform&) = delete;
    WidgetTransform(WidgetTransform&&) = delete;
    WidgetTransform& operator=(WidgetTransform&&) = delete;

    bool Attach(uint32_t id, std::shared_ptr<Frame> presentation,
        std::shared_ptr<Frame> layout, Callbacks callbacks, void* context);
    // Rotation permission follows from the configuration alone, so it is decided here once.
    void Configure(const WidgetConfiguration& configuration);
    void Detach();
    static void RegisterProperties(WidgetPropertyStore& store);
    // True consumes animation properties. Anchors also retain ordinary widget
    // property handling so renderer updates and repaint effects still run.
    bool ApplyProperty(WidgetPropertyType type, const ConfigValue& value);
    void Synchronize();
    bool SetRotation(int32_t angle);
    // Object-free check for a composite that will drive this widget's rotation.
    static bool CanSetRotation(const WidgetConfiguration& configuration);
    // Optional drawable bounds; otherwise anchors use the presentation frame.
    // Renderers supply this after rendering. BeforeRender releases the old target.
    void SetTargetFrame(std::shared_ptr<Frame> target);
    void BeforeRender();
    void OnRendered();
    void OnProcessingUpdated(bool enabled);
    void UpdateAnchor();

private:
    WidgetAnimation animation_;

    uint32_t id_ = 0;
    std::shared_ptr<Frame> presentation_;
    std::shared_ptr<Frame> layout_;
    std::shared_ptr<Frame> target_;

    Callbacks callbacks_ {};
    void* context_ = nullptr;
    lv_point_t anchor_point_ { -1, -1 };
    bool updating_anchor_ = false;
    std::optional<int32_t> angle_;
    bool rotation_allowed_ = true;
    bool target_pending_ = false;

    static void AnchorGeometryCallback(lv_event_t* event);
};

} // namespace eerie_leap::views::widgets
