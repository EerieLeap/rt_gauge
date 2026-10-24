#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <lvgl.h>

#include "domain/ui_domain/models/widget_configuration.h"
#include "domain/ui_domain/models/widget_position.h"
#include "domain/ui_domain/models/widget_property.h"
#include "domain/ui_domain/models/widget_size.h"
#include "domain/ui_domain/models/widget_type.h"
#include "views/i_renderable.h"
#include "views/utilitites/grid_layout.h"

namespace eerie_leap::views::widgets {

using eerie_leap::domain::ui_domain::models::WidgetConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetPosition;
using eerie_leap::domain::ui_domain::models::WidgetPropertyType;
using eerie_leap::domain::ui_domain::models::WidgetSize;
using eerie_leap::domain::ui_domain::models::WidgetType;

class IWidget : public virtual IRenderable {
public:
    using Children = std::vector<std::unique_ptr<IWidget>>;

    ~IWidget() override = default;

    virtual WidgetType GetType() const = 0;
    virtual uint32_t GetId() const = 0;
    virtual bool IsVisible() const = 0;

    // Absolute signed tenths of a degree; full turns wrap. Retained before rendering
    // and while suspended. False rejects a conflicting rotation animation configuration.
    virtual bool SetRotation(int32_t angle) = 0;

    // Lifecycle - a widget on a hidden screen group must not animate or repaint.
    virtual void OnActivated() = 0;
    virtual void OnDeactivated() = 0;
    // A root refreshes its owned subtree. Adoption removes the child's independent
    // display refresh; Detach permanently stops dispatch before destruction.
    virtual void Synchronize() = 0;
    virtual void OnParentAttached() = 0;
    virtual void Detach() = 0;

    // After configuration preflight, build children directly under this stable
    // mount and configure them separately. Transfer ownership once, before
    // configuring/rendering the receiver; later changes require reconstruction.
    virtual std::shared_ptr<Frame> GetChildMount() const = 0;
    virtual void SetChildren(Children children) = 0;
    virtual std::span<const std::unique_ptr<IWidget>> GetChildren() const = 0;

    // Configuration
    virtual void Configure(std::shared_ptr<WidgetConfiguration> configuration) = 0;
    virtual std::shared_ptr<WidgetConfiguration> GetConfiguration() const = 0;

    // The editor changed this configured root widget's record, and nothing in it that ClassifyWidgetChange() says
    // needs a rebuild: every property that differs from the current record is set to its new value, or back to its
    // registered default when the new record leaves it out, through the path an inbound binding takes, and the box
    // is placed again if the position or size changed. The widget object, its LVGL objects and its subscriptions
    // stay. Throws std::logic_error for a record of another widget or type.
    virtual void ApplyEditorConfiguration(std::shared_ptr<WidgetConfiguration> configuration) = 0;
    virtual bool IsSmoothed() const = 0;

    // Configurable keys for this widget, without requiring configuration first.
    // Owned children expose their own keys independently; composites additionally
    // report CHILD_WIDGET_IDS as structural metadata.
    virtual std::vector<WidgetPropertyType> GetSupportedProperties() const = 0;

    // Makes this a root widget placed by the grid of its screen, before it is configured: it then answers
    // for LAYOUT_X, LAYOUT_Y, LAYOUT_WIDTH and LAYOUT_HEIGHT (the box in grid cells, defaulting to the
    // record's), and a change of one moves or sizes it in place. An owned child never has it.
    virtual void AttachGrid(const eerie_leap::views::utilitites::GridLayout& layout) = 0;

    // Layout
    virtual WidgetPosition GetPositionPx() const = 0;
    virtual void SetPositionPx(const WidgetPosition& pos) = 0;
    virtual WidgetSize GetSizePx() const = 0;
    virtual void SetSizePx(const WidgetSize& size) = 0;
};

} // namespace eerie_leap::views::widgets
