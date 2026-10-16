#include <atomic>
#include <memory>
#include <span>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_display_config/cbor_display_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "subsys/device_tree/dt_display.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/threading/work_queue_thread.h"

#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/display_domain/configuration/display_configuration_manager.h"
#include "domain/display_domain/services/display_service.h"

using eerie_leap::configuration::services::CborConfigurationService;
using eerie_leap::domain::configuration_domain::services::ConfigurationService;
using eerie_leap::domain::display_domain::configuration::DisplayConfigurationManager;
using eerie_leap::domain::display_domain::services::DisplayService;
using eerie_leap::subsys::device_tree::DtDisplay;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::threading::WorkQueueThread;

namespace {

constexpr int CONFIG_WORK_QUEUE_STACK_SIZE = 8192;
constexpr int CONFIG_WORK_QUEUE_PRIORITY = 10;
constexpr uint8_t PERSISTED_BRIGHTNESS = 123;

// Returns once everything queued before the call has run.
bool FlushQueue(WorkQueueThread& queue) {
    auto done = std::make_shared<k_sem>();
    k_sem_init(done.get(), 0, 1);

    queue.Run([done] { k_sem_give(done.get()); });

    return k_sem_take(done.get(), K_SECONDS(5)) == 0;
}

} // namespace

ZTEST_SUITE(display_service_persistence, NULL, NULL, NULL, NULL, NULL);

// The brightness the gauge's own slider sets is persisted as the display configuration, but it is the
// unit's own setting: the configuration service reports nothing, so it stops no logging network-wide
// (L9). A display configuration written from outside is reported.
ZTEST(display_service_persistence, test_the_brightness_the_unit_persists_is_stored_and_reported_to_nobody) {
    DtDisplay::Initialize();
    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    fs_service->Format();

    auto queue = std::make_shared<WorkQueueThread>(
        "display_persist_wq", CONFIG_WORK_QUEUE_STACK_SIZE, CONFIG_WORK_QUEUE_PRIORITY);
    zassert_true(queue->Initialize(), "Expected the config work queue to start.");

    auto manager = std::make_shared<DisplayConfigurationManager>(
        std::make_unique<CborConfigurationService<CborDisplayConfig>>("display_config", fs_service, queue));

    auto configuration_service = std::make_shared<ConfigurationService>();
    configuration_service->RegisterCborConfigurationManager(ConfigurationService::Type::Display, manager);

    auto reports = std::make_shared<std::atomic<int>>(0);
    configuration_service->RegisterConfigurationStoredHandler([reports](ConfigurationService::Type) { (*reports)++; });

    {
        DisplayService service(manager, queue);
        zassert_equal(service.Initialize(), 0, "Expected the display service to initialize.");

        zassert_equal(service.SetBrightness(PERSISTED_BRIGHTNESS), 0);
        zassert_equal(service.Persist(), 0);
        zassert_true(FlushQueue(*queue), "The persist job ran");
    }

    zassert_equal(manager->Get(true)->brightness, PERSISTED_BRIGHTNESS, "The brightness is stored");
    zassert_equal(reports->load(), 0, "and the unit's own setting is no configuration update");

    const auto cbor = configuration_service->GetCborConfiguration(ConfigurationService::Type::Display);
    zassert_true(configuration_service->ApplyCborConfiguration(
        ConfigurationService::Type::Display, std::span<const uint8_t>(cbor.data(), cbor.size())));
    zassert_equal(reports->load(), 1, "while a configuration written to the unit is");

    configuration_service->RegisterConfigurationStoredHandler(nullptr);
    queue->Stop();
}
