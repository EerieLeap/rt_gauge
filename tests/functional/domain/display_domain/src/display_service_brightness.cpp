#include <array>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_display_config/cbor_display_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "subsys/device_tree/dt_display.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/event_bus/event.h"
#include "subsys/event_bus/event_bus.h"
#include "subsys/event_bus/event_channel.h"
#include "subsys/event_bus/event_channel_id.h"
#include "subsys/event_bus/event_channel_registry.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/threading/work_queue_thread.h"
#include "utilities/memory/memory_resource_manager.h"
#include "utilities/string/string_helpers.h"

#include "domain/display_domain/configuration/display_configuration_manager.h"
#include "domain/display_domain/models/display_configuration.h"
#include "domain/display_domain/services/display_service.h"
#include "domain/sensor_domain/event_bus/sensor_events_channel.h"
#include "domain/settings_domain/event_bus/settings_events_channel.h"
#include "domain/settings_domain/models/setting_id.h"

using eerie_leap::configuration::services::CborConfigurationService;
using eerie_leap::domain::display_domain::configuration::DisplayConfigurationManager;
using eerie_leap::domain::display_domain::models::DisplayConfiguration;
using eerie_leap::domain::display_domain::services::DisplayService;
using eerie_leap::domain::sensor_domain::event_bus::SensorEventsChannel;
using eerie_leap::domain::sensor_domain::event_bus::SensorEventType;
using eerie_leap::domain::sensor_domain::event_bus::SensorPayloadType;
using eerie_leap::domain::settings_domain::event_bus::SettingsEventsChannel;
using eerie_leap::domain::settings_domain::event_bus::SettingsEventType;
using eerie_leap::domain::settings_domain::event_bus::SettingsPayloadType;
using eerie_leap::domain::settings_domain::models::SettingId;
using eerie_leap::subsys::device_tree::DtDisplay;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::event_bus::EventBus;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::subsys::event_bus::EventChannelRegistry;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::utilities::string::StringHelpers;

namespace {

constexpr std::string_view SENSOR_ID = "ambient_light";
constexpr std::string_view OTHER_SENSOR_ID = "screen_dimmer";
constexpr uint32_t SOURCE_ID = 0x7E57;
constexpr int DISPATCH_TIMEOUT_MS = 1000;
constexpr int NO_DISPATCH_TIMEOUT_MS = 200;
constexpr int BUS_STACK_SIZE = 4096;
constexpr int WORK_QUEUE_STACK_SIZE = 8192;
constexpr int WORK_QUEUE_PRIORITY = 10;

// The channels are inert until a bus drains them, so the suite owns one for the two the service uses.
class TestEventBus : public EventBus {
public:
    TestEventBus() :
        EventBus("display_service_test_bus", BUS_STACK_SIZE) {
        RegisterChannel(SensorEventsChannel::GetInstance());
        RegisterChannel(SettingsEventsChannel::GetInstance());
    }
};

// The configuration work queue the service persists on, and the configuration files are saved on. One
// thread for the whole suite, its stack from the external PMR as EventBus takes its own.
std::shared_ptr<WorkQueueThread> SharedWorkQueue() {
    static std::shared_ptr<WorkQueueThread> queue = [] {
        auto created = std::make_shared<WorkQueueThread>(
            "display_service_test_wq", WORK_QUEUE_STACK_SIZE, WORK_QUEUE_PRIORITY, false, Mrm::GetExtPmr()
        );

        created->Initialize();

        return created;
    }();

    return queue;
}

// Returns once everything queued before the call has run.
bool FlushQueue(WorkQueueThread& queue) {
    auto done = std::make_shared<k_sem>();
    k_sem_init(done.get(), 0, 1);

    queue.Run([done] {
        k_sem_give(done.get());
    });

    return k_sem_take(done.get(), K_SECONDS(5)) == 0;
}

uint32_t BrightnessId() {
    return StringHelpers::GetHash(SettingId::DISPLAY_BRIGHTNESS);
}

// What a widget bound to the brightness setting sees: the values of its Changed events, in order.
class ChangedProbe {
public:
    ChangedProbe() :
        state_(std::make_shared<State>()) {
        k_msgq_init(&state_->queue, state_->storage.data(), sizeof(int), CAPACITY);

        subscription_ = CreateScopedSubscription(
            SettingsEventsChannel::GetInstance(), SettingsEventType::Changed,
            [state = state_](const SettingsEventsChannel::EventMessage& event) {
                const auto* const id = event.payload.find(SettingsPayloadType::SettingId);
                const auto* const value = event.payload.find(SettingsPayloadType::Value);
                if(id == event.payload.end() || value == event.payload.end())
                    return;

                const auto* setting_id = std::get_if<uint32_t>(&id->second);
                const auto* brightness = std::get_if<int>(&value->second);
                if(setting_id == nullptr || *setting_id != BrightnessId() || brightness == nullptr)
                    return;

                const int delivered = *brightness;
                k_msgq_put(&state->queue, &delivered, K_NO_WAIT);
            }
        );
    }

    [[nodiscard]] bool IsSubscribed() const {
        return subscription_ != nullptr;
    }

    // The next value, or none when no Changed event arrives in time.
    std::optional<int> Next() {
        int value = 0;
        if(k_msgq_get(&state_->queue, &value, K_MSEC(DISPATCH_TIMEOUT_MS)) != 0)
            return std::nullopt;

        return value;
    }

    // Whether the service published nothing while a Changed event had every chance to arrive.
    bool WaitForNoEvent() {
        int value = 0;

        return k_msgq_get(&state_->queue, &value, K_MSEC(NO_DISPATCH_TIMEOUT_MS)) != 0;
    }

private:
    static constexpr uint32_t CAPACITY = 16;

    struct State {
        k_msgq queue{};
        alignas(int) std::array<char, CAPACITY * sizeof(int)> storage{};
    };

    std::shared_ptr<State> state_;
    AnySubscription subscription_;
};

DisplayConfiguration MakeConfiguration(uint8_t brightness, std::string_view sensor_id = {}) {
    DisplayConfiguration configuration;
    configuration.brightness = brightness;
    configuration.brightness_sensor_id = sensor_id;

    return configuration;
}

// A display service over a freshly formatted file system, started on the configuration it is given as
// the stored one. The probe sees what the service tells the UI, the initial value included, which is
// taken here.
class Harness {
public:
    std::shared_ptr<DisplayConfigurationManager> manager;
    ChangedProbe changed;
    std::unique_ptr<DisplayService> service;

    explicit Harness(const DisplayConfiguration& stored) {
        DtDisplay::Initialize();
        DtFs::InitInternalFs();

        auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
        fs_service->Format();

        manager = std::make_shared<DisplayConfigurationManager>(
            std::make_unique<CborConfigurationService<CborDisplayConfig>>(
                "display_config", fs_service, SharedWorkQueue()
            )
        );
        zassert_true(manager->Update(stored), "Expected the configuration to be stored.");

        zassert_true(changed.IsSubscribed());

        service = std::make_unique<DisplayService>(manager, SharedWorkQueue());
        zassert_equal(service->Initialize(), 0, "Expected the display service to initialize.");

        const auto initial = changed.Next();
        zassert_true(initial.has_value(), "The service publishes the brightness it starts with.");
        zassert_equal(*initial, static_cast<int>(stored.brightness));
    }

    // The configuration as it is on flash.
    [[nodiscard]] uint8_t StoredBrightness() const {
        return manager->Get(true)->brightness;
    }

    // A sensor's committed value, as the sensor events publisher publishes it. Published in place, so the
    // service has handled it when this returns.
    static void PublishSensorValue(std::string_view sensor_id, const EventData& value) {
        SensorEventsChannel::GetInstance().Publish(
            { .source_id = SOURCE_ID,
                .type = SensorEventType::DataUpdated,
                .payload = { { SensorPayloadType::SensorId, StringHelpers::GetHash(sensor_id) },
                    { SensorPayloadType::Value, value } } }
        );
    }

    // What a widget bound to the setting sends when the person moves it.
    static void RequestBrightness(int brightness) {
        SettingsEventsChannel::GetInstance().Publish(
            { .source_id = SOURCE_ID,
                .type = SettingsEventType::ChangeRequested,
                .payload = {
                    { SettingsPayloadType::SettingId, BrightnessId() }, { SettingsPayloadType::Value, brightness } } }
        );
    }

    // What SettingsPersistenceService requests once a second has passed without a change, and the write
    // the service makes of it, finished.
    static void RequestPersist() {
        SettingsEventsChannel::GetInstance().Publish(
            { .source_id = SOURCE_ID,
                .type = SettingsEventType::PersistRequested,
                .payload = { { SettingsPayloadType::SettingId, BrightnessId() } } }
        );

        zassert_true(FlushQueue(*SharedWorkQueue()), "Expected the persist request to have run.");
    }
};

void* StartEventBus() {
    static TestEventBus bus;

    // What an event binding resolves the sensors channel through.
    EventChannelRegistry::GetInstance().Register(EventChannelId::Sensors, SensorEventsChannel::GetInstance());

    return nullptr;
}

// Lets what the last test published drain before the next one subscribes.
void SettleEventBus(void*) {
    k_msleep(NO_DISPATCH_TIMEOUT_MS);
}

} // namespace

ZTEST_SUITE(display_service_brightness, NULL, StartEventBus, NULL, SettleEventBus, NULL);

ZTEST(display_service_brightness, test_the_panel_starts_at_the_stored_brightness) {
    Harness harness(MakeConfiguration(42, SENSOR_ID));

    zassert_equal(harness.service->GetBrightness(), 42);
}

ZTEST(display_service_brightness, test_a_named_sensors_value_sets_the_brightness_and_is_published) {
    Harness harness(MakeConfiguration(42, SENSOR_ID));

    Harness::PublishSensorValue(SENSOR_ID, 100.0F);

    zassert_equal(harness.service->GetBrightness(), 100);
    zassert_equal(harness.changed.Next(), std::optional<int>(100), "Expected the value as it settled.");

    // An int is a number too.
    Harness::PublishSensorValue(SENSOR_ID, 77);

    zassert_equal(harness.service->GetBrightness(), 77);
    zassert_equal(harness.changed.Next(), std::optional<int>(77));
}

ZTEST(display_service_brightness, test_a_followed_value_is_clamped_like_a_request) {
    Harness harness(MakeConfiguration(42, SENSOR_ID));

    Harness::PublishSensorValue(SENSOR_ID, 3.0F);

    zassert_equal(harness.service->GetBrightness(), 10, "The floor keeps the panel findable.");
    zassert_equal(harness.changed.Next(), std::optional<int>(10));

    Harness::PublishSensorValue(SENSOR_ID, 1000.0F);

    zassert_equal(harness.service->GetBrightness(), 255);
    zassert_equal(harness.changed.Next(), std::optional<int>(255));

    // It is the clamped value that must have moved: this one settles where the panel already is.
    Harness::PublishSensorValue(SENSOR_ID, 5000.0F);

    zassert_equal(harness.service->GetBrightness(), 255);
    zassert_true(harness.changed.WaitForNoEvent(), "A value that moved nothing is no fact.");
}

ZTEST(display_service_brightness, test_another_sensors_value_is_not_followed) {
    Harness harness(MakeConfiguration(42, SENSOR_ID));

    Harness::PublishSensorValue(OTHER_SENSOR_ID, 100.0F);

    zassert_equal(harness.service->GetBrightness(), 42);
    zassert_true(harness.changed.WaitForNoEvent());
}

ZTEST(display_service_brightness, test_with_no_sensor_named_nothing_is_followed) {
    Harness harness(MakeConfiguration(42));

    Harness::PublishSensorValue(SENSOR_ID, 100.0F);
    Harness::PublishSensorValue(OTHER_SENSOR_ID, 100.0F);

    zassert_equal(harness.service->GetBrightness(), 42);
    zassert_true(harness.changed.WaitForNoEvent());
}

ZTEST(display_service_brightness, test_a_value_that_is_not_a_number_is_ignored) {
    Harness harness(MakeConfiguration(42, SENSOR_ID));

    Harness::PublishSensorValue(SENSOR_ID, std::numeric_limits<float>::quiet_NaN());
    Harness::PublishSensorValue(SENSOR_ID, std::string("bright"));

    zassert_equal(harness.service->GetBrightness(), 42);
    zassert_true(harness.changed.WaitForNoEvent());
}

ZTEST(display_service_brightness, test_a_value_published_on_the_bus_is_followed) {
    Harness harness(MakeConfiguration(42, SENSOR_ID));

    // As the sensor events publisher publishes it: queued, and delivered on the bus worker.
    SensorEventsChannel::GetInstance().PublishAsync(
        { .source_id = SOURCE_ID,
            .type = SensorEventType::DataUpdated,
            .payload = { { SensorPayloadType::SensorId, StringHelpers::GetHash(SENSOR_ID) },
                { SensorPayloadType::Value, 120.0F } } }
    );

    zassert_equal(harness.changed.Next(), std::optional<int>(120));
    zassert_equal(harness.service->GetBrightness(), 120);
}

// The request after a quiet second finds nothing to write: the brightness on the panel is not the stored one.
ZTEST(display_service_brightness, test_a_followed_value_is_never_written) {
    Harness harness(MakeConfiguration(42, SENSOR_ID));

    // The stored configuration changes behind the service's back: whatever the service wrote would undo it.
    zassert_true(harness.manager->Update(MakeConfiguration(77, SENSOR_ID)));

    Harness::PublishSensorValue(SENSOR_ID, 100.0F);
    zassert_equal(harness.service->GetBrightness(), 100);

    Harness::RequestPersist();

    zassert_equal(harness.StoredBrightness(), 77, "Nothing was written.");
}

ZTEST(display_service_brightness, test_a_manual_change_applies_and_is_written) {
    Harness harness(MakeConfiguration(42, SENSOR_ID));

    Harness::RequestBrightness(200);

    zassert_equal(harness.service->GetBrightness(), 200);
    zassert_equal(harness.changed.Next(), std::optional<int>(200));
    zassert_equal(harness.StoredBrightness(), 42, "It is written with the persist request, not before.");

    Harness::RequestPersist();

    zassert_equal(harness.StoredBrightness(), 200);
}

ZTEST(display_service_brightness, test_the_next_sensor_value_moves_the_panel_and_not_the_stored_brightness) {
    Harness harness(MakeConfiguration(42, SENSOR_ID));

    Harness::RequestBrightness(200);
    zassert_equal(harness.changed.Next(), std::optional<int>(200));
    Harness::RequestPersist();
    zassert_equal(harness.StoredBrightness(), 200);

    Harness::PublishSensorValue(SENSOR_ID, 90.0F);

    zassert_equal(harness.service->GetBrightness(), 90, "The latest change wins.");
    zassert_equal(harness.changed.Next(), std::optional<int>(90));

    // The stored brightness is still the person's, and a persist request writes nothing over it.
    zassert_true(harness.manager->Update(MakeConfiguration(55, SENSOR_ID)));
    Harness::RequestPersist();

    zassert_equal(harness.StoredBrightness(), 55, "Nothing was written.");
}

// A request for the value a sensor put on the panel moves nothing on the panel, but it is the person's
// choice and becomes the stored brightness.
ZTEST(display_service_brightness, test_a_manual_change_to_the_value_a_sensor_set_is_stored) {
    Harness harness(MakeConfiguration(42, SENSOR_ID));

    Harness::PublishSensorValue(SENSOR_ID, 100.0F);
    zassert_equal(harness.changed.Next(), std::optional<int>(100));

    Harness::RequestBrightness(100);

    zassert_equal(harness.service->GetBrightness(), 100);
    zassert_equal(harness.changed.Next(), std::optional<int>(100), "The persist debounce is re-armed.");

    Harness::RequestPersist();

    zassert_equal(harness.StoredBrightness(), 100);
}

ZTEST(display_service_brightness, test_a_reload_with_another_sensor_follows_the_new_sensor) {
    Harness harness(MakeConfiguration(42, SENSOR_ID));

    Harness::PublishSensorValue(SENSOR_ID, 100.0F);
    zassert_equal(harness.service->GetBrightness(), 100);
    zassert_equal(harness.changed.Next(), std::optional<int>(100));

    // A client's apply stores the configuration, then has the service reload it.
    zassert_true(harness.manager->Update(MakeConfiguration(60, OTHER_SENSOR_ID)));
    zassert_equal(harness.service->Reload(), 0);

    zassert_equal(harness.service->GetBrightness(), 60, "The stored brightness is on the panel until a value comes.");
    zassert_equal(harness.changed.Next(), std::optional<int>(60));

    Harness::PublishSensorValue(SENSOR_ID, 150.0F);

    zassert_equal(harness.service->GetBrightness(), 60, "The old sensor is not followed any more.");
    zassert_true(harness.changed.WaitForNoEvent());

    Harness::PublishSensorValue(OTHER_SENSOR_ID, 180.0F);

    zassert_equal(harness.service->GetBrightness(), 180);
    zassert_equal(harness.changed.Next(), std::optional<int>(180));
}

ZTEST(display_service_brightness, test_a_reload_that_names_the_same_sensor_keeps_following_it) {
    Harness harness(MakeConfiguration(42, SENSOR_ID));

    zassert_true(harness.manager->Update(MakeConfiguration(60, SENSOR_ID)));
    zassert_equal(harness.service->Reload(), 0);
    zassert_equal(harness.changed.Next(), std::optional<int>(60));

    Harness::PublishSensorValue(SENSOR_ID, 130.0F);

    zassert_equal(harness.service->GetBrightness(), 130);
    zassert_equal(harness.changed.Next(), std::optional<int>(130));
}

ZTEST(display_service_brightness, test_a_reload_without_a_sensor_stops_following) {
    Harness harness(MakeConfiguration(42, SENSOR_ID));

    zassert_true(harness.manager->Update(MakeConfiguration(60)));
    zassert_equal(harness.service->Reload(), 0);
    zassert_equal(harness.changed.Next(), std::optional<int>(60));

    Harness::PublishSensorValue(SENSOR_ID, 150.0F);

    zassert_equal(harness.service->GetBrightness(), 60);
    zassert_true(harness.changed.WaitForNoEvent());

    // And one that names a sensor again follows it.
    zassert_true(harness.manager->Update(MakeConfiguration(60, SENSOR_ID)));
    zassert_equal(harness.service->Reload(), 0);
    zassert_equal(harness.changed.Next(), std::optional<int>(60));

    Harness::PublishSensorValue(SENSOR_ID, 150.0F);

    zassert_equal(harness.service->GetBrightness(), 150);
}

// What a client applied is on flash, and replaces a manual change that was still waiting to be written.
ZTEST(display_service_brightness, test_a_reload_takes_the_configuration_unmarked) {
    Harness harness(MakeConfiguration(42));

    Harness::RequestBrightness(200);
    zassert_equal(harness.changed.Next(), std::optional<int>(200));

    zassert_true(harness.manager->Update(MakeConfiguration(77)));
    zassert_equal(harness.service->Reload(), 0);
    zassert_equal(harness.service->GetBrightness(), 77);

    zassert_true(harness.manager->Update(MakeConfiguration(88)));
    Harness::RequestPersist();

    zassert_equal(harness.StoredBrightness(), 88, "Nothing was written.");
}
