#pragma once

#include <cstdint>

#include "subsys/event_bus/event_binding.h"

#include "property_binding_direction.h"
#include "widget_property.h"

namespace eerie_leap::domain::ui_domain::models {

using eerie_leap::subsys::event_bus::EventBinding;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::utilities::type::ConfigValue;

struct PropertyBinding {
    WidgetPropertyType target = WidgetPropertyType::NONE;

    // What an inbound binding reads; an outbound one writes to the same channel, payload key and
    // selector, under outbound_event_type.
    EventBinding event;

    PropertyBindingDirection direction = PropertyBindingDirection::In;

    // The event published when the user moves the widget. Unused by an In binding.
    uint32_t outbound_event_type = 0;
};

} // namespace eerie_leap::domain::ui_domain::models
