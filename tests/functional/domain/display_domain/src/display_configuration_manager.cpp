#include <array>
#include <memory>
#include <span>
#include <vector>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_display_config/cbor_display_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/display_domain/configuration/display_configuration_manager.h"
#include "domain/display_domain/models/display_configuration.h"

#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"

using eerie_leap::configuration::cbor::CborSerializer;
using eerie_leap::configuration::services::CborConfigurationService;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::domain::configuration_domain::services::ConfigurationService;
using eerie_leap::domain::display_domain::configuration::DisplayConfigurationManager;
using eerie_leap::domain::display_domain::models::DisplayConfiguration;

namespace {

std::shared_ptr<DisplayConfigurationManager> MakeManager() {
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());

    fs_service->Format();

    auto cbor_display_configuration_service =
        std::make_unique<CborConfigurationService<CborDisplayConfig>>("display_config", fs_service);

    return std::make_shared<DisplayConfigurationManager>(
        std::move(cbor_display_configuration_service));
}

DisplayConfiguration MakeConfiguration() {
    DisplayConfiguration configuration;
    configuration.brightness = 42;
    configuration.blanking_enabled = true;
    configuration.screen_timeout_s = 90;
    configuration.theme_id = 4;

    return configuration;
}

} // namespace

ZTEST_SUITE(display_configuration_manager, NULL, NULL, NULL, NULL, NULL);

ZTEST(display_configuration_manager, test_an_empty_filesystem_yields_the_default_configuration) {
    auto manager = MakeManager();

    auto configuration = manager->Get();

    zassert_not_null(configuration.get());
    zassert_equal(configuration->brightness, 160);
    zassert_false(configuration->blanking_enabled);
}

ZTEST(display_configuration_manager, test_update_round_trips_through_flash) {
    auto manager = MakeManager();

    zassert_true(manager->Update(MakeConfiguration()));

    auto stored = manager->Get(true);

    zassert_equal(stored->brightness, 42);
    zassert_true(stored->blanking_enabled);
    zassert_equal(stored->screen_timeout_s, 90);
    zassert_equal(stored->theme_id, 4);
}

ZTEST(display_configuration_manager, test_cbor_configuration_round_trips) {
    auto manager = MakeManager();

    zassert_true(manager->Update(MakeConfiguration()));

    auto cbor = manager->GetCborConfiguration();
    zassert_true(cbor.size() > 0);

    DisplayConfiguration replacement;
    replacement.brightness = 7;
    zassert_true(manager->Update(replacement));
    zassert_equal(manager->Get(true)->brightness, 7);

    zassert_true(manager->ApplyCborConfiguration(std::span<const uint8_t>(cbor.data(), cbor.size())));

    auto restored = manager->Get(true);
    zassert_equal(restored->brightness, 42);
    zassert_true(restored->blanking_enabled);
    zassert_equal(restored->screen_timeout_s, 90);
}

// A configuration pushed from outside has to reach the driver without a reboot.
ZTEST(display_configuration_manager, test_apply_cbor_configuration_notifies_the_handler) {
    auto manager = MakeManager();

    zassert_true(manager->Update(MakeConfiguration()));
    auto cbor = manager->GetCborConfiguration();

    int notifications = 0;
    manager->RegisterConfigurationUpdatedHandler([&notifications] { notifications++; });

    // A local update is the service telling the manager what it already applied.
    zassert_true(manager->Update(MakeConfiguration()));
    zassert_equal(notifications, 0);

    zassert_true(manager->ApplyCborConfiguration(std::span<const uint8_t>(cbor.data(), cbor.size())));
    zassert_equal(notifications, 1);
}

ZTEST(display_configuration_manager, test_a_throwing_handler_does_not_fail_the_apply) {
    auto manager = MakeManager();

    zassert_true(manager->Update(MakeConfiguration()));
    auto cbor = manager->GetCborConfiguration();

    manager->RegisterConfigurationUpdatedHandler([] { throw std::runtime_error("boom"); });

    zassert_true(manager->ApplyCborConfiguration(std::span<const uint8_t>(cbor.data(), cbor.size())));
}

ZTEST(display_configuration_manager, test_garbage_cbor_is_rejected) {
    auto manager = MakeManager();

    zassert_true(manager->Update(MakeConfiguration()));

    std::vector<uint8_t> garbage = {0xFF, 0xFF, 0xFF, 0xFF};

    std::array<char, 96> reason{};
    zassert_false(manager->ApplyCborConfiguration(std::span<const uint8_t>(garbage.data(), garbage.size()), reason));
    zassert_str_equal(reason.data(), "Malformed CBOR");
    zassert_equal(manager->Get(true)->brightness, 42);
}

ZTEST(display_configuration_manager, test_a_rejected_configuration_reports_why) {
    auto manager = MakeManager();
    zassert_true(manager->Update(MakeConfiguration()));

    CborSerializer<CborDisplayConfig> serializer;
    const auto exported = manager->GetCborConfiguration();
    auto cbor_config = serializer.Deserialize(exported);
    zassert_not_null(cbor_config.get());
    cbor_config->brightness = 300;

    int notifications = 0;
    manager->RegisterConfigurationUpdatedHandler([&notifications] { notifications++; });

    ConfigurationService configuration_service;
    configuration_service.RegisterCborConfigurationManager(ConfigurationService::Type::Display, manager);

    std::array<char, 96> reason{};
    zassert_false(configuration_service.ApplyCborConfiguration(
        ConfigurationService::Type::Display, serializer.Serialize(*cbor_config), reason));
    zassert_str_equal(reason.data(), "Invalid Display configuration. Brightness must be between 0 and 255. Value: 300.");
    zassert_equal(notifications, 0);
    zassert_equal(manager->Get(true)->brightness, 42);
}

ZTEST(display_configuration_manager, test_the_configuration_service_routes_the_display_type) {
    auto manager = MakeManager();
    zassert_true(manager->Update(MakeConfiguration()));

    ConfigurationService configuration_service;
    configuration_service.RegisterCborConfigurationManager(ConfigurationService::Type::Display, manager);

    auto cbor = configuration_service.GetCborConfiguration(ConfigurationService::Type::Display);
    zassert_true(cbor.size() > 0);

    DisplayConfiguration replacement;
    replacement.brightness = 9;
    zassert_true(manager->Update(replacement));

    zassert_true(configuration_service.ApplyCborConfiguration(
        ConfigurationService::Type::Display,
        std::span<const uint8_t>(cbor.data(), cbor.size())));

    zassert_equal(manager->Get(true)->brightness, 42);
}
