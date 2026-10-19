#pragma once

#include <cstddef>

#include "subsys/event_bus/event_channel.h"

#include "navigation_event_type.h"
#include "navigation_payload_type.h"

namespace eerie_leap::domain::ui_domain::event_bus {

using eerie_leap::subsys::event_bus::EventChannel;

class NavigationEventChannel : public EventChannel<NavigationEventType, NavigationPayloadType> {
private:
    static constexpr size_t MAX_QUEUED_EVENTS = 16;

    NavigationEventChannel() :
        EventChannel("navigation_events", LAST_NAVIGATION_EVENT_TYPE, LAST_NAVIGATION_PAYLOAD_TYPE, MAX_QUEUED_EVENTS) {
    }

public:
    static NavigationEventChannel& GetInstance() {
        static NavigationEventChannel channel;

        return channel;
    }
};

} // namespace eerie_leap::domain::ui_domain::event_bus
