#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <lvgl.h>

#include "domain/ui_domain/models/widget_type.h"
#include "domain/ui_domain/models/widget_position.h"
#include "domain/ui_domain/models/widget_size.h"
#include "domain/ui_domain/models/widget_configuration.h"
#include "domain/ui_domain/models/widget_property.h"
#include "views/i_renderable.h"

namespace eerie_leap::views::widgets {

using eerie_leap::domain::ui_domain::models::WidgetConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetPosition;
using eerie_leap::domain::ui_domain::models::WidgetPropertyType;
using eerie_leap::domain::ui_domain::models::WidgetSize;
using eerie_leap::domain::ui_domain::models::WidgetType;

class IWidget : public virtual IRenderable {
public:
    virtual ~IWidget() = default;

    virtual WidgetType GetType() const = 0;
    virtual uint32_t GetId() const = 0;
    virtual bool IsVisible() const = 0;

    // Absolute signed tenths of a degree; full turns wrap. Retained before rendering
    // and while suspended. False rejects a conflicting rotation animation configuration.
    virtual bool SetRotation(int32_t angle) = 0;

    // Lifecycle - a widget on a hidden screen group must not animate or repaint.
    virtual void OnActivated() = 0;
    virtual void OnDeactivated() = 0;

    // Configuration
    virtual void Configure(std::shared_ptr<WidgetConfiguration> configuration) = 0;
    virtual std::shared_ptr<WidgetConfiguration> GetConfiguration() const = 0;
    virtual bool IsSmoothed() const = 0;

    // Every property a configuration for this widget may carry: what it reads itself, plus what it
    // forwards to a part it builds. Answers "what can I configure here?" without an instance
    // having been configured, and is derived from the registration itself so it cannot drift.
    virtual std::vector<WidgetPropertyType> GetSupportedProperties() const = 0;

    // Layout
    virtual WidgetPosition GetPositionPx() const = 0;
    virtual void SetPositionPx(const WidgetPosition& pos) = 0;
    virtual WidgetSize GetSizePx() const = 0;
    virtual void SetSizePx(const WidgetSize& size) = 0;
};

} // namespace eerie_leap::views::widgets
