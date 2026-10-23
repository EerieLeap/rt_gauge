#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <eerie_memory.hpp>
#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt_defines.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_serializer.h"
#include "configuration/cbor/cbor_ui_config/cbor_ui_config.h"
#include "utilities/memory/memory_resource_manager.h"

#include "subsys/assets/assets_manager.h"
#include "subsys/device_tree/dt_display.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/event_bus/event_bus.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/threading/work_queue_thread.h"

#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/configuration_domain/smp/config_mgmt_group.h"
#include "domain/settings_domain/event_bus/settings_events_channel.h"
#include "domain/ui_domain/configuration/parsers/ui_configuration_cbor_parser.h"
#include "domain/ui_domain/event_bus/navigation_event_channel.h"
#include "domain/ui_domain/models/screen_configuration.h"
#include "domain/ui_domain/models/ui_configuration.h"
#include "domain/ui_domain/models/widget_configuration.h"
#include "domain/ui_preview_domain/smp/ui_mgmt_group.h"

#include "controllers/ui_controller.h"

#include "smp_test_client.h"

using eerie_leap::configuration::cbor::CborSerializer;
using eerie_leap::controllers::UiController;
using eerie_leap::domain::configuration_domain::services::ConfigurationService;
using eerie_leap::domain::configuration_domain::smp::ConfigMgmtGroup;
using eerie_leap::domain::settings_domain::event_bus::SettingsEventsChannel;
using eerie_leap::domain::ui_domain::configuration::parsers::UiConfigurationCborParser;
using eerie_leap::domain::ui_domain::event_bus::NavigationEventChannel;
using eerie_leap::domain::ui_domain::models::ScreenConfiguration;
using eerie_leap::domain::ui_domain::models::ScreenType;
using eerie_leap::domain::ui_domain::models::UiConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetType;
using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::subsys::device_tree::DtDisplay;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::event_bus::EventBus;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::smp::SmpGroupId;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::utilities::memory::Mrm;

using smp_test::Bytes;
using smp_test::CborMap;
using smp_test::Response;

#ifdef CONFIG_EERIE_LEAP_UI_PREVIEW
using eerie_leap::domain::ui_preview_domain::smp::UiMgmtGroup;
#endif

namespace {

constexpr int BUS_STACK_SIZE = 4096;
// The stack main() gives the unit's configuration work queue.
constexpr int CONFIG_WORK_QUEUE_STACK_SIZE = 8192;
constexpr int CONFIG_WORK_QUEUE_PRIORITY = 10;

constexpr int LEASE_MS = CONFIG_EERIE_LEAP_UI_PREVIEW_LEASE_MS;
constexpr int SETTLE_MS = 300;
constexpr int WAIT_MS = 5000;
constexpr int POLL_MS = 20;
constexpr size_t CHUNK_SIZE = 500;

constexpr uint32_t STORED_GROUP_ID = 5;
constexpr uint32_t GROUP_ID = 11;
constexpr uint32_t SECOND_GROUP_ID = 12;
constexpr uint32_t OVERLAY_SCREEN_ID = 40;

// The group's errors, as the plan's UiError.
constexpr uint32_t NO_SESSION = 2;
constexpr uint32_t BAD_TOKEN = 3;
constexpr uint32_t BUSY = 4;
constexpr uint32_t STALE = 5;
constexpr uint32_t TOO_LARGE = 7;
constexpr uint32_t BAD_OFFSET = 8;
constexpr uint32_t CRC_MISMATCH = 9;
constexpr uint32_t INVALID = 10;
constexpr uint32_t NOT_FOUND = 11;

enum class Command : uint8_t {
    BEGIN = 0,
    STAGE = 1,
    PATCH = 2,
    SHOW = 3,
    HIGHLIGHT = 4,
    END = 5,
    STATUS = 6,
};

class SuiteBus : public EventBus {
public:
    SuiteBus() :
        EventBus("smp_ui_test_bus", BUS_STACK_SIZE) {
        RegisterChannel(NavigationEventChannel::GetInstance());
        RegisterChannel(SettingsEventsChannel::GetInstance());
    }
};

struct Fixture {
    std::shared_ptr<FsService> fs_service;
    std::shared_ptr<WorkQueueThread> config_work_queue;
    std::shared_ptr<ConfigurationService> configuration_service = std::make_shared<ConfigurationService>();
    std::shared_ptr<UiController> controller;
    std::shared_ptr<eerie_leap::domain::ui_domain::configuration::UiConfigurationManager> manager;
    std::shared_ptr<eerie_leap::domain::ui_domain::services::NavigationService> navigation;
    std::atomic<int> stored_count = 0;
#ifdef CONFIG_EERIE_LEAP_UI_PREVIEW
    std::unique_ptr<UiMgmtGroup> group;
#endif
    // The `config` group, which a client uploads a UI configuration through.
    std::unique_ptr<ConfigMgmtGroup> config_group;
    std::unique_ptr<smp_test::SmpTestClient> client;
};

// The suite's one fixture lives as long as the process; the constructor allocates nothing that a failure could leak.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
Fixture fixture;

std::shared_ptr<UiConfiguration> MakeConfiguration(
    std::initializer_list<uint32_t> group_ids,
    std::optional<uint32_t> overlay_screen_id = std::nullopt,
    uint32_t widget_count = 1
) {
    auto configuration = eerie_memory::make_shared_pmr<UiConfiguration>(Mrm::GetExtPmr());
    configuration->active_screen_group_id = *group_ids.begin();

    const auto add_screen = [&configuration, widget_count](uint32_t screen_id, uint32_t group_id, bool is_overlay) {
        auto screen = eerie_memory::make_shared_pmr<ScreenConfiguration>(Mrm::GetExtPmr());
        screen->id = screen_id;
        screen->screen_group_id = group_id;
        screen->is_visible = true;
        screen->is_overlay = is_overlay;
        screen->type = is_overlay ? ScreenType::Settings : ScreenType::Gauge;
        screen->grid = { .snap_enabled = true, .width = 466, .height = 466, .spacing_px = 0 };

        for(uint32_t widget_id = 0; widget_id < widget_count; ++widget_id) {
            auto widget = eerie_memory::make_shared_pmr<WidgetConfiguration>(Mrm::GetExtPmr());
            widget->type = WidgetType::IndicatorBar;
            widget->id = widget_id;
            widget->position_grid = { 0, 0 };
            widget->size_grid = { 100, 20 };
            screen->AddWidget(std::move(widget));
        }

        configuration->screen_configurations.push_back(std::move(screen));
    };

    uint32_t screen_id = 0;
    for(const auto group_id : group_ids)
        add_screen(screen_id++, group_id, false);

    if(overlay_screen_id.has_value())
        add_screen(*overlay_screen_id, 0, true);

    return configuration;
}

std::vector<uint8_t> Encode(const std::shared_ptr<UiConfiguration>& configuration) {
    const auto cbor_configuration = UiConfigurationCborParser().Serialize(*configuration);
    const auto bytes = CborSerializer<CborUiConfig>().Serialize(*cbor_configuration);

    return { bytes.begin(), bytes.end() };
}

uint32_t Crc(std::span<const uint8_t> bytes) {
    return crc32_ieee(bytes.data(), bytes.size());
}

Response Write(Command command, const Bytes& body) {
    return fixture.client->Write(SmpGroupId::UI, std::to_underlying(command), body);
}

Response Read(Command command) {
    return fixture.client->Read(SmpGroupId::UI, std::to_underlying(command));
}

// The group's error of a response, 0 for none.
uint32_t ErrorOf(const Response& response) {
    if(!response.error.has_value())
        return 0;

    zassert_equal(
        response.error_group.value_or(0), static_cast<uint32_t>(SmpGroupId::UI), "The error is the ui group's"
    );

    return *response.error;
}

uint32_t Begin() {
    const auto response = Write(Command::BEGIN, CborMap().Build());
    zassert_true(response.IsOk(), "BEGIN refused: %u", ErrorOf(response));

    return response.Value("tok");
}

Response
StageChunk(uint32_t token, std::span<const uint8_t> bytes, size_t offset, std::optional<uint32_t> rev = std::nullopt) {
    const size_t size = std::min(CHUNK_SIZE, bytes.size() - offset);
    CborMap body;
    body.Put("tok", token).Put("off", static_cast<uint32_t>(offset)).Put("data", bytes.subspan(offset, size));
    if(offset == 0) {
        body.Put("len", static_cast<uint32_t>(bytes.size())).Put("crc", Crc(bytes));
        if(rev.has_value())
            body.Put("rev", *rev);
    }

    return Write(Command::STAGE, body.Build());
}

// All of the chunks; the response to the last, or to the first that failed.
Response StageAll(uint32_t token, std::span<const uint8_t> bytes, std::optional<uint32_t> rev = std::nullopt) {
    Response response;
    for(size_t offset = 0; offset < bytes.size(); offset += CHUNK_SIZE) {
        response = StageChunk(token, bytes, offset, rev);
        if(!response.IsOk())
            return response;

        zassert_equal(response.Value("off"), std::min(offset + CHUNK_SIZE, bytes.size()));
    }

    return response;
}

bool WaitUntilGroupIs(uint32_t group_id) {
    const int64_t deadline = k_uptime_get() + WAIT_MS;
    while(fixture.navigation->GetActiveGroupId() != group_id && k_uptime_get() < deadline)
        k_msleep(POLL_MS);

    return fixture.navigation->GetActiveGroupId() == group_id;
}

bool WaitUntilInactive() {
    const int64_t deadline = k_uptime_get() + LEASE_MS + WAIT_MS;
    while(Read(Command::STATUS).Flag("active") && k_uptime_get() < deadline)
        k_msleep(POLL_MS);

    return !Read(Command::STATUS).Flag("active");
}

void* Setup() {
    static SuiteBus bus;

    DtDisplay::Initialize();
    DtFs::InitInternalFs();
    fixture.fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    fixture.fs_service->Format();

    fixture.config_work_queue = std::make_shared<WorkQueueThread>(
        "cfg_work_q", CONFIG_WORK_QUEUE_STACK_SIZE, CONFIG_WORK_QUEUE_PRIORITY
    );
    zassert_true(fixture.config_work_queue->Initialize());

    fixture.configuration_service->RegisterConfigurationStoredHandler([](ConfigurationService::Type) {
        ++fixture.stored_count;
    });
    fixture.controller = std::make_shared<UiController>(
        fixture.fs_service, fixture.config_work_queue, fixture.configuration_service, std::make_shared<AssetsManager>()
    );
    zassert_equal(fixture.controller->Initialize(), 0);
    fixture.controller->Start();
    fixture.manager = fixture.controller->GetConfigurationManager();
    fixture.navigation = fixture.controller->GetNavigationService();

    // What the unit stores, and shows.
    zassert_true(fixture.manager->Update(MakeConfiguration({ STORED_GROUP_ID, 6 })));
    zassert_true(WaitUntilGroupIs(STORED_GROUP_ID));

#ifdef CONFIG_EERIE_LEAP_UI_PREVIEW
    const auto preview = fixture.controller->GetPreviewService();
    zassert_not_null(preview.get());
    fixture.group = std::make_unique<UiMgmtGroup>(preview);
    fixture.group->Register();
#endif
    fixture.config_group = std::make_unique<ConfigMgmtGroup>(fixture.configuration_service, fixture.config_work_queue);
    zassert_true(fixture.config_group->Initialize());
    fixture.config_group->Register();
    fixture.client = std::make_unique<smp_test::SmpTestClient>();

    return nullptr;
}

void Teardown(void*) {
    fixture.client.reset();
    fixture.config_group.reset();
#ifdef CONFIG_EERIE_LEAP_UI_PREVIEW
    fixture.group.reset();
#endif
    fixture.configuration_service->RegisterConfigurationStoredHandler(nullptr);
    fixture.manager.reset();
    fixture.navigation.reset();
    fixture.controller.reset();
    fixture.config_work_queue.reset();
    fixture.fs_service.reset();
}

} // namespace

#ifdef CONFIG_EERIE_LEAP_UI_PREVIEW

namespace {

// A session a failed test left open is ended, and the stored screens are back for the next.
void After(void*) {
    if(Read(Command::STATUS).Flag("active")) {
        fixture.controller->GetPreviewService()->OnLinkDropped();
        zassert_true(WaitUntilInactive());
    }
}

void Before(void*) {
    k_msleep(POLL_MS);
}

} // namespace

ZTEST_SUITE(smp_ui, NULL, Setup, Before, After, Teardown);

// ======== A UI configuration uploaded through the `config` group ========

namespace {

constexpr uint32_t UI_CONFIG_TYPE = 6;
constexpr uint32_t CONFIG_APPLY_FAILED = 8;
constexpr uint32_t CONFIG_CRC_MISMATCH = 7;

// The config group's WRITE in chunks, as a client sends it; the response to the last, or to the first that failed.
Response ConfigWriteAll(uint32_t type, std::span<const uint8_t> bytes, std::optional<uint32_t> crc = std::nullopt) {
    Response response;
    uint32_t token = 0;
    for(size_t offset = 0; offset < bytes.size(); offset += CHUNK_SIZE) {
        const size_t size = std::min(CHUNK_SIZE, bytes.size() - offset);
        CborMap body;
        body.Put("type", type).Put("off", static_cast<uint32_t>(offset)).Put("data", bytes.subspan(offset, size));
        if(offset == 0)
            body.Put("len", static_cast<uint32_t>(bytes.size())).Put("crc", crc.value_or(Crc(bytes)));
        else
            body.Put("tok", token);

        response = fixture.client->Write(
            SmpGroupId::CONFIG, std::to_underlying(ConfigMgmtGroup::Command::WRITE), body.Build()
        );
        if(response.error.has_value() || response.values.contains("rc"))
            return response;

        if(offset == 0)
            token = response.Value("tok");
    }

    return response;
}

void AssertConfigError(const Response& response, uint32_t error, const char* message) {
    zassert_true(response.error.has_value(), "Expected the config group's error %u", error);
    zassert_equal(response.error_group.value_or(0), static_cast<uint32_t>(SmpGroupId::CONFIG));
    zassert_equal(*response.error, error, "Expected config error %u, got %u", error, *response.error);
    if(message != nullptr) {
        zassert_true(response.texts.contains("msg"), "Expected a reason");
        zassert_equal(response.texts.at("msg"), message, "'%s'", response.texts.at("msg").c_str());
    }
}

// The bytes of a configuration that carries more widgets in a screen than the model takes.
std::vector<uint8_t> EncodeWithWidgets(uint32_t widget_count) {
    const auto configuration = MakeConfiguration({ GROUP_ID }, std::nullopt, 1);
    const auto cbor_configuration = UiConfigurationCborParser().Serialize(*configuration);

    // Past the model, which refuses it: copies of the first widget under other IDs.
    auto& widgets = cbor_configuration->CborScreenConfig_m.front().CborWidgetConfig_m;
    for(uint32_t id = 1; id < widget_count; ++id) {
        CborWidgetConfig widget(std::allocator_arg, Mrm::GetExtPmr());
        widget.id = id;
        widget.type = widgets.front().type;
        widget.position = widgets.front().position;
        widget.size = widgets.front().size;
        widget.z_index = 0;
        widget.properties_present = false;
        widgets.push_back(std::move(widget));
    }

    const auto bytes = CborSerializer<CborUiConfig>().Serialize(*cbor_configuration);

    return { bytes.begin(), bytes.end() };
}

} // namespace

ZTEST(smp_ui, test_a_refused_ui_upload_answers_apply_failed_with_the_reason_and_the_next_one_is_not_busy) {
    const auto stored_info = fixture.manager->GetCborConfigurationInfo();
    const int stored_count = fixture.stored_count.load();

    // The second screen takes the first one's ID.
    auto duplicate = Encode(MakeConfiguration({ GROUP_ID, SECOND_GROUP_ID }));
    constexpr std::array<uint8_t, 7> SECOND_SCREEN = { 0x9f, 0x01, SECOND_GROUP_ID, 0x02, 0x00, 0xf5, 0xf4 };
    const auto second_screen = std::ranges::search(duplicate, SECOND_SCREEN);
    zassert_false(second_screen.empty());
    *(second_screen.begin() + 1) = 0x00;

    AssertConfigError(
        ConfigWriteAll(UI_CONFIG_TYPE, duplicate), CONFIG_APPLY_FAILED,
        "Invalid UI Screen configuration. Screen ID: 0. Configuration cannot contain duplicate screen IDs."
    );
    // The same again, straight after: the refused one left no session open.
    AssertConfigError(
        ConfigWriteAll(UI_CONFIG_TYPE, duplicate), CONFIG_APPLY_FAILED,
        "Invalid UI Screen configuration. Screen ID: 0. Configuration cannot contain duplicate screen IDs."
    );

    // More widgets than a screen takes.
    AssertConfigError(
        ConfigWriteAll(UI_CONFIG_TYPE, EncodeWithWidgets(33)), CONFIG_APPLY_FAILED,
        "Invalid UI Screen configuration. Screen ID: 0. Composition exceeds the limit of 32 nodes."
    );

    // Bytes that are no configuration, and a CRC that is not theirs.
    AssertConfigError(
        ConfigWriteAll(UI_CONFIG_TYPE, std::vector<uint8_t>(40, 0xFF)), CONFIG_APPLY_FAILED, "Malformed CBOR"
    );
    const auto good = Encode(MakeConfiguration({ GROUP_ID }));
    AssertConfigError(ConfigWriteAll(UI_CONFIG_TYPE, good, Crc(good) ^ 1U), CONFIG_CRC_MISMATCH, nullptr);

    // Nothing was stored or reported, and a good one is taken.
    zassert_equal(fixture.manager->GetCborConfigurationInfo().crc, stored_info.crc);
    zassert_equal(fixture.stored_count.load(), stored_count);
    const auto taken = ConfigWriteAll(UI_CONFIG_TYPE, good);
    zassert_true(taken.IsOk(), "Refused: %u", taken.error.value_or(0));
    zassert_equal(fixture.stored_count.load(), stored_count + 1);
    zassert_true(WaitUntilGroupIs(GROUP_ID));

    // Back to what the other tests expect.
    zassert_true(fixture.manager->Update(MakeConfiguration({ STORED_GROUP_ID, 6 })));
    zassert_true(WaitUntilGroupIs(STORED_GROUP_ID));
}

ZTEST(smp_ui, test_begin_opens_a_session_and_says_what_the_unit_has) {
    const auto stored_crc = fixture.manager->GetCborConfigurationInfo().crc;

    const auto response = Write(Command::BEGIN, CborMap().Build());

    zassert_true(response.IsOk());
    zassert_not_equal(response.Value("tok"), 0U);
    zassert_equal(response.Value("lease_ms"), static_cast<uint32_t>(LEASE_MS));
    zassert_equal(response.Value("rev"), 0U);
    zassert_equal(response.Value("stored_crc"), stored_crc);
    zassert_equal(response.Value("max_patch"), 0U);
    zassert_equal(response.Value("max_stage"), static_cast<uint32_t>(CONFIG_EERIE_LEAP_UI_PREVIEW_MAX_STAGE_SIZE));
    zassert_false(fixture.controller->IsNavigationEnabled(), "pin defaults to true");
}

ZTEST(smp_ui, test_pin_false_leaves_the_swipes_alone) {
    const auto response = Write(Command::BEGIN, CborMap().PutBool("pin", false).Build());

    zassert_true(response.IsOk());
    zassert_true(fixture.controller->IsNavigationEnabled());
}

ZTEST(smp_ui, test_a_begin_sent_again_gets_the_same_token_and_another_client_is_busy) {
    const auto token = Begin();

    zassert_equal(Begin(), token, "The answer got lost");

    const auto renewed = Write(Command::BEGIN, CborMap().Put("tok", token).Build());
    zassert_true(renewed.IsOk());
    zassert_equal(renewed.Value("tok"), token);

    const auto busy = Write(Command::BEGIN, CborMap().Build());
    zassert_false(busy.IsOk());
    zassert_equal(ErrorOf(busy), BUSY);

    const auto wrong = Write(Command::BEGIN, CborMap().Put("tok", token + 1).Build());
    zassert_equal(ErrorOf(wrong), BAD_TOKEN);
}

// The client that holds a token holds a session that ended: it learns that, and is not given another.
ZTEST(smp_ui, test_a_begin_with_a_token_when_no_session_is_open_is_refused) {
    const auto refused = Write(Command::BEGIN, CborMap().Put("tok", 7).Build());
    zassert_false(refused.IsOk());
    zassert_equal(ErrorOf(refused), NO_SESSION);
    zassert_false(Read(Command::STATUS).Flag("active"), "No session was quietly opened");

    const auto token = Begin();
    zassert_true(Write(Command::END, CborMap().Put("tok", token).Build()).IsOk());

    zassert_equal(ErrorOf(Write(Command::BEGIN, CborMap().Put("tok", token).Build())), NO_SESSION);
    zassert_false(Read(Command::STATUS).Flag("active"));
}

ZTEST(smp_ui, test_requests_need_the_session_and_its_token) {
    const auto show = CborMap().Put("tok", 1).PutMap("target", "group", GROUP_ID).Build();
    const auto highlight = CborMap().Put("tok", 1).Build();
    const auto end = CborMap().Put("tok", 1).Build();
    const std::vector<uint8_t> bytes = { 1, 2, 3 };

    zassert_equal(ErrorOf(Write(Command::SHOW, show)), NO_SESSION);
    zassert_equal(ErrorOf(Write(Command::HIGHLIGHT, highlight)), NO_SESSION);
    zassert_equal(ErrorOf(Write(Command::END, end)), NO_SESSION);
    zassert_equal(ErrorOf(StageChunk(1, bytes, 0)), NO_SESSION);

    const auto token = Begin();
    zassert_equal(
        ErrorOf(Write(Command::SHOW, CborMap().Put("tok", token + 1).PutMap("target", "group", GROUP_ID).Build())),
        BAD_TOKEN
    );
    zassert_equal(ErrorOf(Write(Command::HIGHLIGHT, CborMap().Put("tok", token + 1).Build())), BAD_TOKEN);
    zassert_equal(ErrorOf(Write(Command::END, CborMap().Put("tok", token + 1).Build())), BAD_TOKEN);
    zassert_equal(ErrorOf(StageChunk(token + 1, bytes, 0)), BAD_TOKEN);
    zassert_true(Read(Command::STATUS).Flag("active"), "A wrong token ends nothing");
}

ZTEST(smp_ui, test_a_request_missing_a_key_is_refused_as_malformed) {
    const auto token = Begin();

    const auto stage = Write(Command::STAGE, CborMap().Put("tok", token).Put("off", 0).Build());
    zassert_false(stage.IsOk());
    zassert_equal(stage.Value("rc"), static_cast<uint32_t>(MGMT_ERR_EINVAL));

    // The first chunk says what the transfer is.
    const std::vector<uint8_t> bytes = { 1, 2, 3 };
    const auto first = Write(Command::STAGE, CborMap().Put("tok", token).Put("off", 0).Put("data", bytes).Build());
    zassert_equal(first.Value("rc"), static_cast<uint32_t>(MGMT_ERR_EINVAL));

    zassert_equal(Write(Command::END, CborMap().Build()).Value("rc"), static_cast<uint32_t>(MGMT_ERR_EINVAL));
    zassert_equal(
        Write(Command::SHOW, CborMap().Put("tok", token).Put("target", 3).Build()).Value("rc"),
        static_cast<uint32_t>(MGMT_ERR_EINVAL), "A target is a map of a group or an overlay"
    );
}

ZTEST(smp_ui, test_stage_in_chunks_shows_the_configuration_and_stores_nothing) {
    const auto stored_info = fixture.manager->GetCborConfigurationInfo();
    const auto stored_bytes = fixture.manager->GetCborConfiguration();
    const int stored_count = fixture.stored_count.load();
    const auto token = Begin();
    constexpr uint32_t MANY_WIDGETS = 30;
    const auto bytes = Encode(MakeConfiguration({ GROUP_ID, SECOND_GROUP_ID }, OVERLAY_SCREEN_ID, MANY_WIDGETS));
    zassert_true(
        bytes.size() > 2 * CHUNK_SIZE, "Expected the transfer to take chunks (%u bytes)",
        static_cast<unsigned>(bytes.size())
    );

    const auto response = StageAll(token, bytes);

    zassert_true(response.IsOk());
    zassert_equal(response.Value("off"), bytes.size());
    zassert_equal(response.Value("rev"), 1U);
    zassert_equal(response.Value("group"), GROUP_ID, "Where the gauge stands: the start group of what is staged");
    zassert_false(response.Has("overlay"));
    zassert_equal(*fixture.navigation->GetActiveGroupId(), GROUP_ID);
    zassert_equal(fixture.navigation->GoToGroup(STORED_GROUP_ID), -ENOENT, "The stored groups are replaced");

    zassert_true(fixture.manager->GetCborConfiguration() == stored_bytes);
    zassert_equal(fixture.manager->GetCborConfigurationInfo().crc, stored_info.crc);
    zassert_equal(fixture.stored_count.load(), stored_count, "Nothing that reacts to a store was told");

    // The earlier chunks did not say the revision.
    const auto status = Read(Command::STATUS);
    zassert_equal(status.Value("rev"), 1U);
}

// The editor changes a screen, not the one the user is looking at.
ZTEST(smp_ui, test_stage_keeps_the_group_and_the_overlay_shown_and_answers_where_the_gauge_stands) {
    const auto token = Begin();
    const auto bytes = Encode(MakeConfiguration({ GROUP_ID, SECOND_GROUP_ID }, OVERLAY_SCREEN_ID));
    zassert_true(StageAll(token, bytes).IsOk());

    zassert_true(
        Write(Command::SHOW, CborMap().Put("tok", token).PutMap("target", "group", SECOND_GROUP_ID).Build()).IsOk()
    );
    zassert_true(WaitUntilGroupIs(SECOND_GROUP_ID));
    zassert_true(
        Write(Command::SHOW, CborMap().Put("tok", token).PutMap("target", "overlay", OVERLAY_SCREEN_ID).Build()).IsOk()
    );
    k_msleep(SETTLE_MS);
    zassert_true(fixture.navigation->IsOverlayActive());

    const auto again = StageAll(token, bytes);
    zassert_true(again.IsOk());
    zassert_equal(again.Value("rev"), 2U);
    zassert_equal(again.Value("group"), SECOND_GROUP_ID);
    zassert_true(again.Has("overlay"));
    zassert_equal(again.Value("overlay"), OVERLAY_SCREEN_ID);
    k_msleep(SETTLE_MS);
    zassert_equal(*fixture.navigation->GetActiveGroupId(), SECOND_GROUP_ID);
    zassert_true(fixture.navigation->IsOverlayActive());

    // A configuration without them: the start group and no overlay.
    const auto other = StageAll(token, Encode(MakeConfiguration({ SECOND_GROUP_ID + 1 })));
    zassert_true(other.IsOk());
    zassert_equal(other.Value("group"), SECOND_GROUP_ID + 1);
    zassert_false(other.Has("overlay"));
}

// The answer to the last chunk got lost, and the client sends it again.
ZTEST(smp_ui, test_a_lost_answer_to_the_last_chunk_is_recovered_from) {
    const auto token = Begin();
    constexpr uint32_t MANY_WIDGETS = 30;
    const auto bytes = Encode(MakeConfiguration({ GROUP_ID }, std::nullopt, MANY_WIDGETS));
    zassert_true(bytes.size() > CHUNK_SIZE);
    zassert_equal(StageAll(token, bytes).Value("rev"), 1U);

    const size_t last_offset = (bytes.size() - 1) / CHUNK_SIZE * CHUNK_SIZE;
    const auto repeated = StageChunk(token, bytes, last_offset);
    zassert_equal(ErrorOf(repeated), BAD_OFFSET);
    zassert_equal(repeated.Value("off"), 0U, "The transfer is over: start from the top");

    // From the top, it is shown again; with the revision the client already has, the unit says it is there.
    zassert_equal(StageAll(token, bytes).Value("rev"), 2U);
    const auto stale = StageAll(token, bytes, 2);
    zassert_equal(ErrorOf(stale), STALE);
    zassert_equal(stale.Value("rev"), 2U);
}

// Judged against the configuration, not against what is built yet.
ZTEST(smp_ui, test_highlight_names_an_overlay_just_shown) {
    const auto token = Begin();
    zassert_true(StageAll(token, Encode(MakeConfiguration({ GROUP_ID }, OVERLAY_SCREEN_ID))).IsOk());
    zassert_true(
        Write(Command::SHOW, CborMap().Put("tok", token).PutMap("target", "overlay", OVERLAY_SCREEN_ID).Build()).IsOk()
    );

    zassert_true(
        Write(Command::HIGHLIGHT, CborMap().Put("tok", token).Put("screen", OVERLAY_SCREEN_ID).Put("widget", 0).Build())
            .IsOk()
    );
    zassert_true(Write(Command::HIGHLIGHT, CborMap().Put("tok", token).Put("widget", 0).Build()).IsOk());
}

// A SHOW moves navigation: with nothing staged, the end takes it back.
ZTEST(smp_ui, test_a_show_alone_is_taken_back_by_the_end) {
    const auto token = Begin();

    zassert_true(Write(Command::SHOW, CborMap().Put("tok", token).PutMap("target", "group", 6).Build()).IsOk());
    zassert_true(WaitUntilGroupIs(6));

    const auto ended = Write(Command::END, CborMap().Put("tok", token).Build());
    zassert_true(ended.IsOk());
    zassert_false(ended.Flag("reverted"));
    zassert_true(WaitUntilGroupIs(STORED_GROUP_ID));
}

ZTEST(smp_ui, test_stage_is_checked_as_an_upload_is) {
    const auto token = Begin();
    const auto good = Encode(MakeConfiguration({ GROUP_ID }));

    // A CRC the bytes do not have.
    CborMap bad_crc;
    bad_crc.Put("tok", token)
        .Put("off", 0)
        .Put("data", good)
        .Put("len", static_cast<uint32_t>(good.size()))
        .Put("crc", Crc(good) ^ 1U);
    zassert_equal(ErrorOf(Write(Command::STAGE, bad_crc.Build())), CRC_MISMATCH);

    // Bytes that are no configuration: INVALID, with the validator's own message.
    const std::vector<uint8_t> garbage(40, 0xFF);
    const auto malformed = StageAll(token, garbage);
    zassert_equal(ErrorOf(malformed), INVALID);
    zassert_true(malformed.texts.contains("msg"));
    zassert_equal(malformed.texts.at("msg"), "Malformed CBOR");

    // A configuration the validator refuses: the second screen takes the first one's ID.
    auto invalid = Encode(MakeConfiguration({ GROUP_ID, SECOND_GROUP_ID }));
    constexpr std::array<uint8_t, 7> SECOND_SCREEN = { 0x9f, 0x01, SECOND_GROUP_ID, 0x02, 0x00, 0xf5, 0xf4 };
    const auto second_screen = std::ranges::search(invalid, SECOND_SCREEN);
    zassert_false(second_screen.empty());
    *(second_screen.begin() + 1) = 0x00;
    const auto refused = StageAll(token, invalid);
    zassert_equal(ErrorOf(refused), INVALID);
    zassert_true(refused.texts.at("msg").contains("duplicate screen IDs"));

    // More than the buffer holds.
    CborMap too_large;
    too_large.Put("tok", token).Put("off", 0).Put("data", std::span(good).first(10));
    too_large.Put("len", static_cast<uint32_t>(CONFIG_EERIE_LEAP_UI_PREVIEW_MAX_STAGE_SIZE) + 1).Put("crc", 0);
    zassert_equal(ErrorOf(Write(Command::STAGE, too_large.Build())), TOO_LARGE);

    // A chunk that does not follow, and where to resume.
    const auto skipped = StageChunk(token, std::vector<uint8_t>(600, 7), 600);
    zassert_equal(ErrorOf(skipped), BAD_OFFSET);
    zassert_equal(skipped.Value("off"), 0U);

    // None of it showed, and the session is still there for the right bytes.
    zassert_equal(*fixture.navigation->GetActiveGroupId(), STORED_GROUP_ID);
    zassert_true(Read(Command::STATUS).Flag("active"));
    zassert_true(StageAll(token, good).IsOk());
    zassert_equal(*fixture.navigation->GetActiveGroupId(), GROUP_ID);
}

ZTEST(smp_ui, test_a_revision_must_go_forward) {
    const auto token = Begin();
    const auto bytes = Encode(MakeConfiguration({ GROUP_ID }));

    const auto first = StageAll(token, bytes, 4);
    zassert_true(first.IsOk());
    zassert_equal(first.Value("rev"), 4U);

    const auto stale = StageAll(token, bytes, 4);
    zassert_equal(ErrorOf(stale), STALE);
    zassert_equal(stale.Value("rev"), 4U);

    const auto next = StageAll(token, bytes);
    zassert_equal(next.Value("rev"), 5U);
}

ZTEST(smp_ui, test_show_names_a_group_or_an_overlay) {
    const auto token = Begin();
    zassert_true(StageAll(token, Encode(MakeConfiguration({ GROUP_ID, SECOND_GROUP_ID }, OVERLAY_SCREEN_ID))).IsOk());

    const auto group = Write(
        Command::SHOW, CborMap().Put("tok", token).PutMap("target", "group", SECOND_GROUP_ID).Build()
    );
    zassert_true(group.IsOk());
    zassert_equal(group.Value("group"), SECOND_GROUP_ID);
    zassert_false(group.Has("overlay"));
    zassert_true(WaitUntilGroupIs(SECOND_GROUP_ID));

    const auto overlay = Write(
        Command::SHOW, CborMap().Put("tok", token).PutMap("target", "overlay", OVERLAY_SCREEN_ID).Build()
    );
    zassert_true(overlay.IsOk());
    zassert_equal(overlay.Value("group"), SECOND_GROUP_ID);
    zassert_equal(overlay.Value("overlay"), OVERLAY_SCREEN_ID);
    k_msleep(SETTLE_MS);
    zassert_true(fixture.navigation->IsOverlayActive());

    zassert_equal(
        ErrorOf(Write(Command::SHOW, CborMap().Put("tok", token).PutMap("target", "group", 99).Build())), NOT_FOUND
    );
    zassert_equal(
        ErrorOf(Write(Command::SHOW, CborMap().Put("tok", token).PutMap("target", "overlay", 99).Build())), NOT_FOUND
    );
}

ZTEST(smp_ui, test_highlight_outlines_a_widget_and_neither_clears_it) {
    const auto token = Begin();
    zassert_true(StageAll(token, Encode(MakeConfiguration({ GROUP_ID }))).IsOk());

    zassert_true(
        Write(Command::HIGHLIGHT, CborMap().Put("tok", token).Put("screen", 0).Put("widget", 0).Build()).IsOk()
    );
    zassert_true(Write(Command::HIGHLIGHT, CborMap().Put("tok", token).Put("widget", 0).Build()).IsOk());
    zassert_equal(
        ErrorOf(Write(Command::HIGHLIGHT, CborMap().Put("tok", token).Put("screen", 0).Put("widget", 99).Build())),
        NOT_FOUND
    );
    zassert_true(Write(Command::HIGHLIGHT, CborMap().Put("tok", token).Build()).IsOk());
}

ZTEST(smp_ui, test_status_needs_no_token_and_tells_the_session) {
    const auto idle = Read(Command::STATUS);
    zassert_true(idle.IsOk());
    zassert_false(idle.Flag("active"));
    zassert_false(idle.Has("rev"));
    zassert_false(idle.Has("lease_left_ms"));
    zassert_equal(idle.Value("stored_crc"), fixture.manager->GetCborConfigurationInfo().crc);
    zassert_equal(idle.Value("group"), STORED_GROUP_ID);

    const auto token = Begin();
    zassert_true(StageAll(token, Encode(MakeConfiguration({ GROUP_ID }))).IsOk());

    const auto active = Read(Command::STATUS);
    zassert_true(active.Flag("active"));
    zassert_equal(active.Value("rev"), 1U);
    zassert_true(active.Value("lease_left_ms") > 0 && active.Value("lease_left_ms") <= static_cast<uint32_t>(LEASE_MS));
    zassert_equal(active.Value("group"), GROUP_ID);
}

ZTEST(smp_ui, test_end_puts_the_stored_screens_back_and_says_whether_it_had_to) {
    const auto first = Begin();
    const auto idle = Write(Command::END, CborMap().Put("tok", first).Build());
    zassert_true(idle.IsOk());
    zassert_false(idle.Flag("reverted"), "A preview that showed nothing reverts nothing");
    zassert_true(fixture.controller->IsNavigationEnabled(), "and releases the pin");

    const auto token = Begin();
    zassert_true(StageAll(token, Encode(MakeConfiguration({ GROUP_ID }))).IsOk());
    zassert_equal(*fixture.navigation->GetActiveGroupId(), GROUP_ID);

    const auto ended = Write(Command::END, CborMap().Put("tok", token).Build());
    zassert_true(ended.IsOk());
    zassert_true(ended.Flag("reverted"));
    zassert_equal(*fixture.navigation->GetActiveGroupId(), STORED_GROUP_ID);
    zassert_false(Read(Command::STATUS).Flag("active"));

    zassert_equal(ErrorOf(Write(Command::END, CborMap().Put("tok", token).Build())), NO_SESSION);
}

ZTEST(smp_ui, test_the_lease_ends_a_session_nobody_renews) {
    const auto token = Begin();
    zassert_true(StageAll(token, Encode(MakeConfiguration({ GROUP_ID }))).IsOk());

    zassert_true(WaitUntilInactive());
    zassert_true(WaitUntilGroupIs(STORED_GROUP_ID));
    zassert_equal(ErrorOf(Write(Command::END, CborMap().Put("tok", token).Build())), NO_SESSION);
}

ZTEST(smp_ui, test_a_stored_configuration_ends_the_session) {
    const auto token = Begin();
    zassert_true(StageAll(token, Encode(MakeConfiguration({ GROUP_ID }))).IsOk());
    constexpr uint32_t UPLOADED_GROUP_ID = 30;

    zassert_true(fixture.manager->Update(MakeConfiguration({ UPLOADED_GROUP_ID })));

    zassert_true(WaitUntilInactive());
    zassert_true(WaitUntilGroupIs(UPLOADED_GROUP_ID));

    // Back to what the other tests expect.
    zassert_true(fixture.manager->Update(MakeConfiguration({ STORED_GROUP_ID, 6 })));
    zassert_true(WaitUntilGroupIs(STORED_GROUP_ID));
}

// PATCH comes with FW-2.
ZTEST(smp_ui, test_patch_is_not_served_yet) {
    const auto token = Begin();

    const auto response = Write(Command::PATCH, CborMap().Put("tok", token).Put("rev", 1).Build());

    zassert_false(response.IsOk());
    zassert_equal(response.Value("rc"), static_cast<uint32_t>(MGMT_ERR_ENOTSUP));
}

ZTEST(smp_ui, test_a_command_that_does_not_exist_is_not_served) {
    const auto response = fixture.client->Write(SmpGroupId::UI, 7, CborMap().Build());

    zassert_false(response.IsOk());
}

#else // CONFIG_EERIE_LEAP_UI_PREVIEW

ZTEST_SUITE(smp_ui, NULL, Setup, NULL, NULL, Teardown);

// A unit built without the preview does not register the group, and MCUmgr answers as it does for any group it
// does not have.
ZTEST(smp_ui, test_without_the_preview_the_group_is_not_served) {
    zassert_is_null(fixture.controller->GetPreviewService().get());

    const auto begin = fixture.client->Write(SmpGroupId::UI, std::to_underlying(Command::BEGIN), CborMap().Build());
    zassert_false(begin.IsOk());
    zassert_equal(begin.Value("rc"), static_cast<uint32_t>(MGMT_ERR_ENOTSUP));

    const auto status = fixture.client->Read(SmpGroupId::UI, std::to_underlying(Command::STATUS));
    zassert_false(status.IsOk());
    zassert_equal(status.Value("rc"), static_cast<uint32_t>(MGMT_ERR_ENOTSUP));
}

#endif // CONFIG_EERIE_LEAP_UI_PREVIEW
