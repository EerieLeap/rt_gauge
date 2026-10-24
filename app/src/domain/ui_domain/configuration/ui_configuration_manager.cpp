#include <exception>

#include "configuration/cbor/cbor_serializer.h"

#include <zephyr/logging/log.h>

#include "ui_configuration_manager.h"

namespace eerie_leap::domain::ui_domain::configuration {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::configuration::services;

LOG_MODULE_REGISTER(ui_configuration_manager_logger);

UiConfigurationManager::UiConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborUiConfig>> cbor_configuration_service,
    UiConfigurationValidator::ChildValidator validate_children,
    EventBindingChecker check_binding
) :
    CborConfigurationManagerBase("UI", std::move(cbor_configuration_service)),
    validate_children_(validate_children),
    cbor_parser_(std::move(validate_children)),
    check_binding_(std::move(check_binding)) {

    LoadOrCreateDefault();
}

void UiConfigurationManager::RegisterConfigurationUpdatedHandler(ConfigurationUpdatedHandler handler) {
    configuration_updated_handler_ = std::move(handler);
}

void UiConfigurationManager::NotifyUpdated() {
    if(!configuration_updated_handler_)
        return;

    try {
        configuration_updated_handler_();
    } catch(const std::exception& e) {
        LOG_ERR("The UI configuration's updated handler failed. %s", e.what());
    } catch(...) {
        LOG_ERR("The UI configuration's updated handler failed.");
    }
}

bool UiConfigurationManager::ApplyCborConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) {
    if(!CborConfigurationManagerBase::ApplyCborConfiguration(cbor_data, reason))
        return false;

    NotifyUpdated();

    return true;
}

bool UiConfigurationManager::Update(std::shared_ptr<UiConfiguration> configuration) {
    if(!Adopt(std::move(configuration)))
        return false;

    NotifyUpdated();

    return true;
}

PmrUniquePtr<CborUiConfig> UiConfigurationManager::Serialize(const UiConfiguration& configuration) {
    return cbor_parser_.Serialize(configuration);
}

PmrUniquePtr<UiConfiguration> UiConfigurationManager::Deserialize(const CborUiConfig& cbor_config) {
    return cbor_parser_.Deserialize(Mrm::GetExtPmr(), cbor_config);
}

void UiConfigurationManager::ValidateApply(const UiConfiguration& configuration) {
    UiConfigurationValidator::ValidateBindings(configuration, check_binding_);
}

void UiConfigurationManager::ValidateConfiguration(const UiConfiguration& configuration) const {
    UiConfigurationValidator::Validate(configuration, validate_children_);
    UiConfigurationValidator::ValidateBindings(configuration, check_binding_);
}

size_t UiConfigurationManager::GetEncodedSize(const UiConfiguration& configuration) {
    try {
        const auto cbor_configuration = cbor_parser_.Serialize(configuration);

        return eerie_leap::configuration::cbor::CborSerializer<CborUiConfig>().Serialize(*cbor_configuration).size();
    } catch(const std::exception& e) {
        LOG_ERR("Failed to encode the UI configuration to measure it. %s", e.what());

        return 0;
    }
}

bool UiConfigurationManager::CreateDefaultConfiguration() {
    return Update(make_shared_pmr<UiConfiguration>(Mrm::GetExtPmr()));
}

} // namespace eerie_leap::domain::ui_domain::configuration
