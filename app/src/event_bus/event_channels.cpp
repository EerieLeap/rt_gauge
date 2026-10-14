#include "subsys/event_bus/event_channel_registry.h"

#include "domain/sensor_domain/event_bus/sensor_event_bus.h"

#include "app_event_bus.h"
#include "event_channels.h"

namespace eerie_leap::event_bus {

using eerie_leap::domain::sensor_domain::event_bus::SensorEventBus;
using eerie_leap::domain::sensor_domain::event_bus::SensorEventsChannel;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::EventChannelRegistry;

void InitializeEventChannels() {
    SensorEventBus::GetInstance();
    AppEventBus::GetInstance();

    auto& registry = EventChannelRegistry::GetInstance();

    registry.Register(EventChannelId::Sensors, SensorEventsChannel::GetInstance());
    registry.Register(EventChannelId::Logging, LoggingEventsChannel::GetInstance());
    registry.Register(EventChannelId::Navigation, NavigationEventChannel::GetInstance());
    registry.Register(EventChannelId::Settings, SettingsEventsChannel::GetInstance());
}

} // namespace eerie_leap::event_bus
