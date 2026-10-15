#pragma once

#include "subsys/event_bus/event_bus.h"

#include "domain/logging_domain/event_bus/logging_events_channel.h"
#include "domain/settings_domain/event_bus/settings_events_channel.h"
#include "domain/ui_domain/event_bus/navigation_event_channel.h"

namespace eerie_leap::event_bus {

using eerie_leap::subsys::event_bus::EventBus;
using eerie_leap::domain::logging_domain::event_bus::LoggingEventsChannel;
using eerie_leap::domain::settings_domain::event_bus::SettingsEventsChannel;
using eerie_leap::domain::ui_domain::event_bus::NavigationEventChannel;

class AppEventBus : public EventBus {
private:
    static constexpr int k_stack_size = 4096;

    AppEventBus() : EventBus("app_event_bus", k_stack_size) {
        RegisterChannel(NavigationEventChannel::GetInstance());
        // rt_core creates no bus for logging_events: the composition root picks one, and on the gauge it
        // is this one. A channel stays with the first bus that registers it; another bus's registration
        // fails with -EEXIST.
        RegisterChannel(LoggingEventsChannel::GetInstance());
        RegisterChannel(SettingsEventsChannel::GetInstance());
    }

public:
    static AppEventBus& GetInstance() {
        static AppEventBus bus;

        return bus;
    }
};

} // namespace eerie_leap::event_bus
