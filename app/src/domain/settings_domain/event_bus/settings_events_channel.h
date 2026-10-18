#pragma once

#include <cstddef>

#include "subsys/event_bus/event_channel.h"

#include "settings_event_type.h"
#include "settings_payload_type.h"

namespace eerie_leap::domain::settings_domain::event_bus {

using eerie_leap::subsys::event_bus::EventChannel;

class SettingsEventsChannel : public EventChannel<SettingsEventType, SettingsPayloadType> {
private:
    static constexpr size_t MAX_QUEUED_EVENTS = 16;

    SettingsEventsChannel() :
        EventChannel("settings_events", MAX_QUEUED_EVENTS) {}

public:
    static SettingsEventsChannel& GetInstance() {
        static SettingsEventsChannel channel;

        return channel;
    }
};

} // namespace eerie_leap::domain::settings_domain::event_bus
