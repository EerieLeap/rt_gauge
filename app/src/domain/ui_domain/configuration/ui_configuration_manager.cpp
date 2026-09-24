#include <zephyr/logging/log.h>

#include "utilities/cbor/cbor_helpers.hpp"
#include "utilities/memory/heap_allocator.h"

#include "ui_configuration_manager.h"

namespace eerie_leap::domain::ui_domain::configuration {

using namespace eerie_memory;
using namespace eerie_leap::utilities::cbor;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::configuration::services;

LOG_MODULE_REGISTER(ui_config_ctrl_logger);

UiConfigurationManager::UiConfigurationManager(
    std::unique_ptr<CborConfigurationService<CborUiConfig>> cbor_configuration_service,
    UiConfigurationValidator::ChildValidator validate_children)
        : cbor_configuration_service_(std::move(cbor_configuration_service)),
        configuration_(nullptr) {

    cbor_parser_ = std::make_unique<UiConfigurationCborParser>(std::move(validate_children));
    std::shared_ptr<UiConfiguration> configuration = nullptr;

    try {
        configuration = Get(true);
    } catch(...) {
        LOG_ERR("Failed to load UI configuration.");
    }

    if(configuration == nullptr) {
        if(!CreateDefaultConfiguration()) {
            LOG_ERR("Failed to create default UI configuration.");
            return;
        }

        LOG_INF("Default UI configuration loaded successfully.");
    }

    LOG_INF("UI Configuration Manager initialized successfully.");
}

bool UiConfigurationManager::ApplyCborConfiguration(std::span<const uint8_t> cbor_data) {
    auto cbor_config = cbor_configuration_service_->Deserialize(cbor_data);
    if(cbor_config == nullptr)
        return false;

    try {
        auto configuration = cbor_parser_->Deserialize(Mrm::GetExtPmr(), *cbor_config);

        // Decoding validated the import, so the received encoding is stored as is.
        if(!cbor_configuration_service_->Save(cbor_config.get()))
            return false;

        configuration_ = std::make_shared<UiConfiguration>(std::move(*configuration));
    } catch(const std::exception& e) {
        LOG_ERR("Failed to deserialize CBOR configuration. %s", e.what());
        return false;
    }

    LOG_INF("CBOR configuration loaded successfully.");

    return true;
}

std::pmr::vector<uint8_t> UiConfigurationManager::GetCborConfiguration() {
    auto cbor_config = cbor_parser_->Serialize(*configuration_);

    return cbor_configuration_service_->Serialize(*cbor_config);
}

bool UiConfigurationManager::Update(std::shared_ptr<UiConfiguration> configuration) {
    try {
        auto cbor_config = cbor_parser_->Serialize(*configuration);

        if(!cbor_configuration_service_->Save(cbor_config.get()))
            return false;
    } catch(const std::exception& e) {
        LOG_ERR("Failed to update UI configuration. %s", e.what());
        return false;
    }

    configuration_ = std::move(configuration);

    return true;
}

std::shared_ptr<UiConfiguration> UiConfigurationManager::Get(bool force_load) {
    if(configuration_ != nullptr && !force_load)
        return configuration_;

    auto cbor_config_data = cbor_configuration_service_->Load();
    if(!cbor_config_data.has_value())
        return nullptr;

    auto cbor_config = std::move(cbor_config_data.value().config);

    auto configuration = cbor_parser_->Deserialize(Mrm::GetExtPmr(), *cbor_config);
    configuration_ = std::make_shared<UiConfiguration>(std::move(*configuration));

    return configuration_;
}

bool UiConfigurationManager::CreateDefaultConfiguration() {
    return Update(make_shared_pmr<UiConfiguration>(Mrm::GetExtPmr()));
}

} // namespace eerie_leap::domain::ui_domain::configuration
