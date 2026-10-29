#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#include <zephyr/logging/log.h>

#include "subsys/event_bus/event_binding.h"
#include "subsys/event_bus/event_channel_id.h"
#include "utilities/memory/memory_resource_manager.h"

#include "domain/sensor_domain/event_bus/sensor_event_type.h"
#include "domain/sensor_domain/event_bus/sensor_payload_type.h"

#include "display_configuration_manager.h"

namespace eerie_leap::domain::display_domain::configuration {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::configuration::services;

using eerie_leap::domain::sensor_domain::event_bus::SensorEventType;
using eerie_leap::domain::sensor_domain::event_bus::SensorPayloadType;
using eerie_leap::subsys::event_bus::EventBinding;
using eerie_leap::subsys::event_bus::EventChannelId;

LOG_MODULE_REGISTER(display_config_ctrl_logger);

DisplayConfigurationManager::DisplayConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborDisplayConfig>> cbor_configuration_service
) :
    CborConfigurationManagerBase("Display", std::move(cbor_configuration_service)) {

    LoadOrCreateDefault();
}

void DisplayConfigurationManager::RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) {
    configuration_updated_handler_ = std::move(handler);
}

void DisplayConfigurationManager::SetBrightnessSensorChecker(EventBindingChecker check_brightness_sensor) {
    check_brightness_sensor_ = std::move(check_brightness_sensor);
}

bool DisplayConfigurationManager::ApplyCborConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) {
    if(!CborConfigurationManagerBase::ApplyCborConfiguration(cbor_data, reason))
        return false;

    // Only the externally supplied configuration needs to be pushed to the
    // driver; a local Update() comes from the service that already applied it.
    if(configuration_updated_handler_) {
        try {
            configuration_updated_handler_();
        } catch(const std::exception& e) {
            LOG_ERR("Display configuration updated handler failed. %s", e.what());
        } catch(...) {
            LOG_ERR("Display configuration updated handler failed.");
        }
    }

    return true;
}

PmrUniquePtr<CborDisplayConfig> DisplayConfigurationManager::Serialize(const DisplayConfiguration& configuration) {
    return DisplayConfigurationCborParser::Serialize(configuration);
}

PmrUniquePtr<DisplayConfiguration> DisplayConfigurationManager::Deserialize(const CborDisplayConfig& cbor_config) {
    return DisplayConfigurationCborParser::Deserialize(Mrm::GetExtPmr(), cbor_config);
}

bool DisplayConfigurationManager::CreateDefaultConfiguration() {
    return Update(DisplayConfiguration{});
}

// Asks about the binding DisplayService subscribes with, so the answer is about the values the service will take.
void DisplayConfigurationManager::ValidateApply(const DisplayConfiguration& configuration) {
    if(configuration.brightness_sensor_id.empty() || !check_brightness_sensor_)
        return;

    EventBinding binding;
    binding.channel = EventChannelId::Sensors;
    binding.event_type = std::to_underlying(SensorEventType::DataUpdated);
    binding.payload_key = std::to_underlying(SensorPayloadType::Value);
    binding.selector_key = std::to_underlying(SensorPayloadType::SensorId);
    binding.selector_value = std::pmr::string(configuration.brightness_sensor_id, Mrm::GetExtPmr());

    const auto error = check_brightness_sensor_(binding);
    if(!error.empty())
        throw std::invalid_argument("Invalid Display configuration. Brightness sensor ID " + std::string(error));
}

} // namespace eerie_leap::domain::display_domain::configuration
