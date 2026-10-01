#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <utility>
#include <vector>

#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include "smp_test_client.h"

#include "subsys/threading/work_queue_thread.h"
#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/configuration_domain/utilities/i_cbor_configuration_manager.h"

#include "controllers/management_controller.h"

using namespace smp_test;
using eerie_leap::controllers::ManagementController;
using eerie_leap::domain::configuration_domain::services::ConfigurationService;
using eerie_leap::domain::configuration_domain::utilities::ICborConfigurationManager;
using eerie_leap::domain::configuration_domain::utilities::StoredCborInfo;
using eerie_leap::subsys::threading::WorkQueueThread;

namespace {

constexpr int CONFIG_WORK_QUEUE_STACK_SIZE = 4096;
constexpr int CONFIG_WORK_QUEUE_PRIORITY = 10;
constexpr uint32_t BUILD_NUMBER = 4321;

class StoredConfigurationManager : public ICborConfigurationManager {
private:
    std::vector<uint8_t> stored_ = {0x80};

public:
    bool ApplyCborConfiguration(std::span<const uint8_t> cbor_data) override {
        stored_.assign(cbor_data.begin(), cbor_data.end());
        return true;
    }

    std::pmr::vector<uint8_t> GetCborConfiguration() override {
        return std::pmr::vector<uint8_t>(stored_.begin(), stored_.end());
    }

    StoredCborInfo GetCborConfigurationInfo() override {
        return {.size = stored_.size(), .crc = crc32_ieee(stored_.data(), stored_.size())};
    }
};

} // namespace

ZTEST_SUITE(management_controller, NULL, NULL, NULL, NULL, NULL);

ZTEST(management_controller, test_serves_the_config_and_device_groups) {
    auto work_queue = std::make_shared<WorkQueueThread>(
        "mgmt_test_config_wq", CONFIG_WORK_QUEUE_STACK_SIZE, CONFIG_WORK_QUEUE_PRIORITY);
    zassert_true(work_queue->Initialize());

    auto configuration_service = std::make_shared<ConfigurationService>();
    configuration_service->RegisterCborConfigurationManager(
        ConfigurationService::Type::Ui, std::make_shared<StoredConfigurationManager>());

    ManagementController controller(configuration_service, work_queue, nullptr, BUILD_NUMBER);
    zassert_equal(controller.Initialize(), 0);

    SmpTestClient client;

    const auto device = client.Read(SmpGroupId::DEVICE, 0);
    zassert_true(device.IsOk());
    zassert_equal(device.Value("build"), BUILD_NUMBER);
    zassert_false(device.Has("cdmp_id"), "No CDMP identity without network info");

    const auto types = client.Read(SmpGroupId::CONFIG, 0);
    zassert_true(types.IsOk());
    zassert_equal(types.entries.size(), 1);
    zassert_equal(types.entries[0].at("type"), std::to_underlying(ConfigurationService::Type::Ui));
}
