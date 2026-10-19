#include "ui_configuration_manager.h"

namespace eerie_leap::domain::ui_domain::configuration {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::configuration::services;

UiConfigurationManager::UiConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborUiConfig>> cbor_configuration_service,
    UiConfigurationValidator::ChildValidator validate_children,
    EventBindingChecker check_binding
) :
    CborConfigurationManagerBase("UI", std::move(cbor_configuration_service)),
    cbor_parser_(std::move(validate_children)),
    check_binding_(std::move(check_binding)) {

    LoadOrCreateDefault();
}

bool UiConfigurationManager::Update(std::shared_ptr<UiConfiguration> configuration) {
    return Adopt(std::move(configuration));
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

bool UiConfigurationManager::CreateDefaultConfiguration() {
    return Update(make_shared_pmr<UiConfiguration>(Mrm::GetExtPmr()));
}

} // namespace eerie_leap::domain::ui_domain::configuration
