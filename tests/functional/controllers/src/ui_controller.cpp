#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <eerie_memory.hpp>
#include <lvgl.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/crc.h>
#include <zephyr/ztest.h>

#include "configuration/cbor/cbor_serializer.h"
#include "configuration/cbor/cbor_ui_config/cbor_ui_config.h"
#include "configuration/services/cbor_configuration_service.h"
#include "utilities/memory/memory_resource_manager.h"

#include "subsys/assets/asset_kind.h"
#include "subsys/assets/asset_provider.h"
#include "subsys/assets/asset_store.h"
#include "subsys/assets/assets_manager.h"
#include "subsys/device_tree/dt_display.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/event_bus/event_bus.h"
#include "subsys/fs/services/fs_service.h"
#include "subsys/threading/work_queue_thread.h"

#include "domain/settings_domain/event_bus/settings_event_type.h"
#include "domain/settings_domain/event_bus/settings_events_channel.h"
#include "domain/ui_domain/configuration/parsers/ui_configuration_cbor_parser.h"
#include "domain/ui_domain/configuration/ui_configuration_manager.h"
#include "domain/ui_domain/event_bus/navigation_event_channel.h"
#include "domain/ui_domain/lvgl_lock.h"
#include "domain/ui_domain/models/navigation_intent.h"
#include "domain/ui_domain/models/screen_configuration.h"
#include "domain/ui_domain/models/ui_configuration.h"
#include "domain/ui_domain/models/widget_configuration.h"
#include "domain/ui_domain/services/navigation_service.h"

#include "controllers/ui_controller.h"
#include "views/assets/animations/animations_register.h"
#include "views/assets/images/images_register.h"

using eerie_leap::configuration::cbor::CborSerializer;
using eerie_leap::configuration::services::CborConfigurationService;
using eerie_leap::controllers::UiController;
using eerie_leap::domain::configuration_domain::services::ConfigurationService;
using eerie_leap::domain::settings_domain::event_bus::SettingsEventsChannel;
using eerie_leap::domain::settings_domain::event_bus::SettingsEventType;
using eerie_leap::domain::ui_domain::ScopedLvglLock;
using eerie_leap::domain::ui_domain::configuration::UiConfigurationManager;
using eerie_leap::domain::ui_domain::configuration::parsers::UiConfigurationCborParser;
using eerie_leap::domain::ui_domain::event_bus::NavigationEventChannel;
using eerie_leap::domain::ui_domain::event_bus::NavigationEventType;
using eerie_leap::domain::ui_domain::event_bus::NavigationPayloadType;
using eerie_leap::domain::ui_domain::models::NavigationAction;
using eerie_leap::domain::ui_domain::models::ScreenConfiguration;
using eerie_leap::domain::ui_domain::models::ScreenType;
using eerie_leap::domain::ui_domain::models::UiConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetType;
using eerie_leap::domain::ui_domain::preview::UiPreviewError;
using eerie_leap::domain::ui_domain::services::NavigationService;
using eerie_leap::domain::ui_preview_domain::UiPreviewService;
using eerie_leap::domain::ui_preview_domain::UiPreviewShown;
using eerie_leap::domain::ui_preview_domain::UiPreviewTarget;
using eerie_leap::subsys::assets::AssetAccess;
using eerie_leap::subsys::assets::AssetKind;
using eerie_leap::subsys::assets::AssetProvider;
using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::subsys::assets::AssetStore;
using eerie_leap::subsys::device_tree::DtDisplay;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::event_bus::EventBus;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::subsys::threading::WorkQueueThread;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::views::assets::animations::UI_LOTTIE_SPINNER_JSON;
using eerie_leap::views::assets::images::UI_IMG_ARROW_AL88_DATA;
using eerie_leap::views::assets::images::UI_IMG_NORMA_AL88_DATA;

namespace {

constexpr int BUS_STACK_SIZE = 4096;
constexpr int CONFIG_WORK_QUEUE_STACK_SIZE = 8192;
constexpr int CONFIG_WORK_QUEUE_PRIORITY = 10;

// The demo configuration UiController installs on every Initialize().
constexpr uint32_t DEMO_OVERLAY_SCREEN_ID = 3;
constexpr uint32_t UNKNOWN_SCREEN_ID = 99;
constexpr uint32_t FIRST_GROUP_ID = 0;

// A navigation change is applied on the bus worker; give it room to run and, for
// the reconcile paths, to publish the follow-up close and apply that too.
constexpr int SETTLE_MS = 300;
constexpr int DISPATCH_TIMEOUT_MS = 1000;

// UiController subscribes to NavigationEventChannel, which stays inert until a
// bus drains it.
class ControllerBus : public EventBus {
public:
    ControllerBus() :
        EventBus("ctrl_test_bus", BUS_STACK_SIZE) {
        RegisterChannel(NavigationEventChannel::GetInstance());
        RegisterChannel(SettingsEventsChannel::GetInstance());
    }
};

#ifdef CONFIG_EERIE_LEAP_UI_DEMO_CONFIGURATION
// One controller for the whole suite: Initialize() builds every screen in the demo
// configuration and writes ~870 KB of assets, and its LVGL objects are process-wide.
struct Fixture {
    std::shared_ptr<FsService> fs_service;
    std::shared_ptr<WorkQueueThread> config_work_queue;
    std::shared_ptr<AssetsManager> assets_manager;
    std::shared_ptr<UiController> controller;
    std::shared_ptr<NavigationService> navigation_service;
};

Fixture fixture;

#endif
void EnsureBus() {
    static ControllerBus bus;
}

#ifdef CONFIG_EERIE_LEAP_UI_DEMO_CONFIGURATION
void* Setup() {
    EnsureBus();

    // UiRendererService reads the display through DtDisplay, which main() primes.
    DtDisplay::Initialize();

    DtFs::InitInternalFs();
    fixture.fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    fixture.fs_service->Format();

    fixture.config_work_queue = std::make_shared<WorkQueueThread>(
        "cfg_work_q", CONFIG_WORK_QUEUE_STACK_SIZE, CONFIG_WORK_QUEUE_PRIORITY
    );
    zassert_true(fixture.config_work_queue->Initialize(), "Expected the config work queue to start.");

    fixture.assets_manager = std::make_shared<AssetsManager>();
    fixture.controller = std::make_shared<UiController>(
        fixture.fs_service, fixture.config_work_queue, nullptr, fixture.assets_manager
    );

    zassert_equal(fixture.controller->Initialize(), 0, "Expected the UI controller to initialize.");
    zassert_equal(fixture.controller->Start(), 0, "Expected the UI controller to start.");

    fixture.navigation_service = fixture.controller->GetNavigationService();
    zassert_not_null(fixture.navigation_service.get(), "Expected a navigation service.");

    return nullptr;
}

// Overlay and group state are process-wide, so each test starts from the same place
// rather than from wherever the previous one left off.
void Reset(void*) {
    if(fixture.navigation_service->IsOverlayActive())
        fixture.navigation_service->CloseOverlay();

    k_msleep(SETTLE_MS);

    fixture.navigation_service->GoToGroup(FIRST_GROUP_ID);
    k_msleep(SETTLE_MS);
}

// Without this the renderer thread keeps the simulation alive after the report.
void Teardown(void*) {
    fixture.controller.reset();
    fixture.navigation_service.reset();
    fixture.assets_manager.reset();
    fixture.config_work_queue.reset();
    fixture.fs_service.reset();
}

NavigationService& Navigation() {
    return *fixture.navigation_service;
}

AssetsManager& Assets() {
    return *fixture.assets_manager;
}

// What a client may do with the gauge's UI assets: read, overwrite and delete them.
constexpr uint8_t UI_ASSETS_ACCESS = ToMask(AssetAccess::READ) | ToMask(AssetAccess::WRITE)
    | ToMask(AssetAccess::DELETE);

#endif
// The demo files, which the kinds keep in directories of their own.
constexpr std::array<std::string_view, 3> DEMO_ASSETS{ "ui_img_norma_al88.bin", "ui_img_arrow_al88.bin",
    "ui_lottie_spinner.json" };

#ifdef CONFIG_EERIE_LEAP_UI_DEMO_CONFIGURATION
void CheckProvider(
    const AssetsManager& assets,
    AssetKind kind,
    std::string_view name,
    std::string_view directory,
    std::string_view extension,
    uint32_t max_size
) {

    const auto provider = assets.Find(kind);
    zassert_not_null(provider.get(), "Expected the %s kind to be registered.", std::string(name).c_str());

    zassert_equal(provider->kind, kind);
    zassert_true(provider->name == name, "Expected the kind to be listed as %s.", std::string(name).c_str());
    zassert_equal(provider->extensions.size(), 1U);
    zassert_true(provider->extensions[0] == extension, "Expected the extension %s.", std::string(extension).c_str());
    zassert_equal(provider->access, UI_ASSETS_ACCESS);
    zassert_equal(provider->max_size, max_size);
    zassert_false(static_cast<bool>(provider->is_in_use), "Expected nothing to be in use: widgets keep copies.");
    zassert_not_null(provider->store.get());
    zassert_true(provider->store->IsAvailable());
    zassert_true(
        provider->store->GetDirectory() == directory, "Expected the %s kind in %s, not in %s.",
        std::string(name).c_str(), std::string(directory).c_str(), provider->store->GetDirectory().c_str()
    );
}

void CheckLoads(const AssetProvider& provider, std::string_view name, std::span<const uint8_t> expected) {
    const auto loaded = provider.store->Load(name);

    zassert_equal(loaded.size(), expected.size(), "Expected %s to load whole.", std::string(name).c_str());
    zassert_true(
        std::equal(loaded.begin(), loaded.end(), expected.begin()), "Expected %s to load as it was written.",
        std::string(name).c_str()
    );
}

// The demo files load through the providers, whole, and are not at the root of ui_assets/, where they used to be.
void CheckDemoAssets(const AssetsManager& assets, const IFsService& fs_service) {
    const auto images = assets.Find(AssetKind::IMAGES);
    const auto animations = assets.Find(AssetKind::ANIMATIONS);
    zassert_not_null(images.get(), "Expected the images to be registered.");
    zassert_not_null(animations.get(), "Expected the animations to be registered.");

    CheckLoads(*images, "ui_img_norma_al88.bin", UI_IMG_NORMA_AL88_DATA);
    CheckLoads(*images, "ui_img_arrow_al88.bin", UI_IMG_ARROW_AL88_DATA);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters viewed as the bytes they are.
    const auto* json_data = reinterpret_cast<const uint8_t*>(UI_LOTTIE_SPINNER_JSON.data());
    CheckLoads(*animations, "ui_lottie_spinner.json", std::span(json_data, UI_LOTTIE_SPINNER_JSON.size()));

    for(const auto name : DEMO_ASSETS) {
        const auto legacy_path = "ui_assets/" + std::string(name);
        zassert_false(fs_service.Exists(legacy_path), "Expected %s to be gone.", legacy_path.c_str());
    }
}

#endif
} // namespace

#ifdef CONFIG_EERIE_LEAP_UI_DEMO_CONFIGURATION
ZTEST_SUITE(ui_controller, NULL, Setup, Reset, Reset, Teardown);

// What a client sees of the gauge's UI assets (the SMP assets group) is these two kinds.
ZTEST(ui_controller, test_the_controller_registers_the_image_and_animation_kinds) {
    CheckProvider(
        Assets(), AssetKind::IMAGES, "images", "ui_assets/images", ".bin", CONFIG_EERIE_LEAP_UI_IMAGE_MAX_SIZE
    );
    CheckProvider(
        Assets(), AssetKind::ANIMATIONS, "animations", "ui_assets/animations", ".json",
        CONFIG_EERIE_LEAP_UI_ANIMATION_MAX_SIZE
    );

    zassert_equal(Assets().GetProviders().size(), 2U, "Expected no kind but those of the UI.");
    zassert_not_equal(
        Assets().Find(AssetKind::IMAGES)->store.get(), Assets().Find(AssetKind::ANIMATIONS)->store.get(),
        "Expected a store for each kind."
    );
}

// The widgets of the demo configuration load these by name, and now through the kinds.
ZTEST(ui_controller, test_the_demo_assets_load_through_the_registered_kinds) {
    CheckDemoAssets(Assets(), *fixture.fs_service);
}

// Configure() reads the stored configuration and must have found usable groups.
ZTEST(ui_controller, test_the_controller_starts_on_a_configured_screen_group) {
    auto active_screen_group_id = Navigation().GetActiveGroupId();

    zassert_true(active_screen_group_id.has_value(), "Expected an active screen group.");
    zassert_equal(*active_screen_group_id, FIRST_GROUP_ID);
    zassert_false(Navigation().IsOverlayActive());
}

// An overlay shares a screen_group_id with a real screen; Configure() must skip it rather
// than render it into that group, and navigation must still be able to move.
ZTEST(ui_controller, test_group_navigation_works_alongside_a_configured_overlay) {
    zassert_equal(Navigation().Next(), 0, "Expected the next group to be reachable.");
    k_msleep(SETTLE_MS);

    auto active_screen_group_id = Navigation().GetActiveGroupId();
    zassert_true(active_screen_group_id.has_value());
    zassert_not_equal(*active_screen_group_id, FIRST_GROUP_ID, "Expected the group to have changed.");

    zassert_equal(Navigation().GoToGroup(FIRST_GROUP_ID), 0);
    k_msleep(SETTLE_MS);
}

// The whole ShowOverlay path: event -> ScreenFactory -> OverlayHost::Push. Any
// failure along it reconciles by closing, so an overlay still active after the
// bus has settled is proof the push succeeded.
ZTEST(ui_controller, test_showing_a_configured_overlay_keeps_it_active) {
    zassert_equal(Navigation().ShowOverlay(DEMO_OVERLAY_SCREEN_ID), 0);
    k_msleep(SETTLE_MS);

    zassert_true(Navigation().IsOverlayActive(), "Expected the overlay to have been pushed.");
}

ZTEST(ui_controller, test_overlay_can_reopen_above_the_shape_screen) {
    // Group 1 contains the large rounded shape. Its initial render runs on the
    // UI work queue; opening the overlay then allocates and redraws above it.
    zassert_equal(Navigation().GoToGroup(1), 0);
    k_msleep(SETTLE_MS);

    for(int i = 0; i < 3; ++i) {
        zassert_equal(Navigation().ShowOverlay(DEMO_OVERLAY_SCREEN_ID), 0);
        k_msleep(SETTLE_MS);
        zassert_true(Navigation().IsOverlayActive());
        zassert_equal(Navigation().CloseOverlay(), 0);
        k_msleep(SETTLE_MS);
        zassert_false(Navigation().IsOverlayActive());
        zassert_equal(*Navigation().GetActiveGroupId(), 1);
    }
}

// An overlay is built long after Configure() broadcast the settings state, so the
// bindings its widgets just resolved have to be seeded again or they keep their defaults.
ZTEST(ui_controller, test_showing_an_overlay_asks_settings_owners_to_republish) {
    k_sem requested{};
    k_sem_init(&requested, 0, K_SEM_MAX_LIMIT);

    AnySubscription subscription = CreateScopedSubscription(
        SettingsEventsChannel::GetInstance(), SettingsEventType::StateRequested,
        [&requested](const SettingsEventsChannel::EventMessage&) {
            k_sem_give(&requested);
        }
    );
    zassert_not_null(subscription.get(), "Expected to subscribe to settings events.");

    zassert_equal(Navigation().ShowOverlay(DEMO_OVERLAY_SCREEN_ID), 0);

    zassert_equal(
        k_sem_take(&requested, K_MSEC(DISPATCH_TIMEOUT_MS)), 0, "Expected the overlay to request the settings state."
    );
}

ZTEST(ui_controller, test_an_active_overlay_blocks_group_navigation) {
    zassert_equal(Navigation().ShowOverlay(DEMO_OVERLAY_SCREEN_ID), 0);
    k_msleep(SETTLE_MS);

    zassert_equal(Navigation().Next(), -EBUSY);
    zassert_equal(Navigation().Previous(), -EBUSY);
    zassert_equal(Navigation().GoToGroup(FIRST_GROUP_ID), -EBUSY);
    zassert_equal(Navigation().ShowOverlay(DEMO_OVERLAY_SCREEN_ID), -EBUSY);
}

ZTEST(ui_controller, test_closing_an_overlay_restores_group_navigation) {
    zassert_equal(Navigation().ShowOverlay(DEMO_OVERLAY_SCREEN_ID), 0);
    k_msleep(SETTLE_MS);

    zassert_equal(Navigation().CloseOverlay(), 0);
    k_msleep(SETTLE_MS);

    zassert_false(Navigation().IsOverlayActive());
    zassert_equal(Navigation().Next(), 0, "Expected group navigation to work again.");
    k_msleep(SETTLE_MS);
}

// A screen id that is not a configured overlay: the service has already taken the
// optimistic state, so the controller has to hand it back or navigation stays stuck.
ZTEST(ui_controller, test_an_unknown_overlay_id_reconciles_navigation) {
    zassert_equal(Navigation().ShowOverlay(UNKNOWN_SCREEN_ID), 0);
    zassert_true(Navigation().IsOverlayActive(), "Expected the request to be taken optimistically.");

    k_msleep(SETTLE_MS);

    zassert_false(Navigation().IsOverlayActive(), "Expected the failed show to close itself.");
    zassert_equal(Navigation().Next(), 0, "Expected group navigation to be unblocked.");
    k_msleep(SETTLE_MS);
}

// A non-overlay screen must not be showable as one, even though its id exists.
ZTEST(ui_controller, test_a_non_overlay_screen_id_reconciles_navigation) {
    zassert_equal(Navigation().ShowOverlay(FIRST_GROUP_ID), 0);
    k_msleep(SETTLE_MS);

    zassert_false(Navigation().IsOverlayActive(), "Expected a plain screen to be refused as an overlay.");
}

// Nothing to close is reported as such, and navigation stays where it is.
ZTEST(ui_controller, test_closing_with_no_overlay_open_is_refused) {
    zassert_equal(Navigation().CloseOverlay(), -ENOENT);
    k_msleep(SETTLE_MS);

    zassert_false(Navigation().IsOverlayActive());
    zassert_true(Navigation().GetActiveGroupId().has_value());
}

// The kinds go with the controller, which the suite above keeps to its end, so this one builds its own.
ZTEST_SUITE(ui_controller_assets, NULL, NULL, NULL, NULL, NULL);

#endif
namespace {

// A freshly formatted volume.
std::shared_ptr<FsService> FormatFs() {
    DtDisplay::Initialize();

    DtFs::InitInternalFs();
    auto fs_service = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    fs_service->Format();

    return fs_service;
}

std::shared_ptr<WorkQueueThread> StartConfigWorkQueue() {
    auto config_work_queue = std::make_shared<WorkQueueThread>(
        "cfg_work_q", CONFIG_WORK_QUEUE_STACK_SIZE, CONFIG_WORK_QUEUE_PRIORITY
    );
    zassert_true(config_work_queue->Initialize(), "Expected the config work queue to start.");

    return config_work_queue;
}

} // namespace

#ifdef CONFIG_EERIE_LEAP_UI_DEMO_CONFIGURATION
ZTEST(ui_controller_assets, test_destroying_the_controller_unregisters_its_asset_kinds) {
    auto fs_service = FormatFs();
    auto config_work_queue = StartConfigWorkQueue();

    auto assets_manager = std::make_shared<AssetsManager>();
    auto controller = std::make_shared<UiController>(fs_service, config_work_queue, nullptr, assets_manager);
    zassert_equal(controller->Initialize(), 0, "Expected the UI controller to initialize.");

    CheckProvider(
        *assets_manager, AssetKind::IMAGES, "images", "ui_assets/images", ".bin", CONFIG_EERIE_LEAP_UI_IMAGE_MAX_SIZE
    );
    CheckProvider(
        *assets_manager, AssetKind::ANIMATIONS, "animations", "ui_assets/animations", ".json",
        CONFIG_EERIE_LEAP_UI_ANIMATION_MAX_SIZE
    );

    controller.reset();

    zassert_is_null(assets_manager->Find(AssetKind::IMAGES).get(), "Expected the images to go with the controller.");
    zassert_is_null(
        assets_manager->Find(AssetKind::ANIMATIONS).get(), "Expected the animations to go with the controller."
    );
    zassert_true(assets_manager->GetProviders().empty());
}

// The demo files used to sit at the root of ui_assets/. Written there first, they are cleared by Initialize(), and
// the kinds' directories hold the bundled files, not the stale ones.
ZTEST(ui_controller_assets, test_the_demo_assets_move_from_the_ui_assets_root_into_the_kind_directories) {
    auto fs_service = FormatFs();
    auto config_work_queue = StartConfigWorkQueue();

    const std::array<uint8_t, 16> stale{};
    zassert_true(fs_service->CreateDirectory("ui_assets"));
    for(const auto name : DEMO_ASSETS) {
        const auto legacy_path = "ui_assets/" + std::string(name);
        zassert_true(fs_service->WriteFile(legacy_path, stale.data(), stale.size()));
        zassert_true(fs_service->Exists(legacy_path), "Expected %s to be written.", legacy_path.c_str());
    }

    auto assets_manager = std::make_shared<AssetsManager>();
    auto controller = std::make_shared<UiController>(fs_service, config_work_queue, nullptr, assets_manager);
    zassert_equal(controller->Initialize(), 0, "Expected the UI controller to initialize.");

    CheckDemoAssets(*assets_manager, *fs_service);
}

#endif
namespace {

// A configuration of one bar on each of the given groups, the first of them the one it starts on, and, when
// asked for, an overlay of one bar.
std::shared_ptr<UiConfiguration>
MakeConfiguration(std::initializer_list<uint32_t> group_ids, std::optional<uint32_t> overlay_screen_id = std::nullopt) {
    auto configuration = eerie_memory::make_shared_pmr<UiConfiguration>(Mrm::GetExtPmr());
    configuration->active_screen_group_id = *group_ids.begin();

    uint32_t screen_id = 0;
    for(const auto group_id : group_ids) {
        auto screen = eerie_memory::make_shared_pmr<ScreenConfiguration>(Mrm::GetExtPmr());
        screen->id = screen_id++;
        screen->screen_group_id = group_id;
        screen->is_visible = true;
        screen->type = ScreenType::Gauge;
        screen->grid = { .snap_enabled = true, .width = 466, .height = 466, .spacing_px = 0 };

        auto widget = eerie_memory::make_shared_pmr<WidgetConfiguration>(Mrm::GetExtPmr());
        widget->type = WidgetType::IndicatorBar;
        widget->id = 0;
        widget->position_grid = { 0, 0 };
        widget->size_grid = { 100, 20 };
        screen->AddWidget(std::move(widget));

        configuration->screen_configurations.push_back(std::move(screen));
    }

    if(overlay_screen_id.has_value()) {
        auto overlay = eerie_memory::make_shared_pmr<ScreenConfiguration>(Mrm::GetExtPmr());
        overlay->id = *overlay_screen_id;
        overlay->screen_group_id = 0;
        overlay->is_visible = true;
        overlay->is_overlay = true;
        overlay->type = ScreenType::Settings;
        overlay->grid = { .snap_enabled = true, .width = 466, .height = 466, .spacing_px = 0 };

        auto widget = eerie_memory::make_shared_pmr<WidgetConfiguration>(Mrm::GetExtPmr());
        widget->type = WidgetType::IndicatorBar;
        widget->id = 0;
        widget->position_grid = { 0, 0 };
        widget->size_grid = { 100, 20 };
        overlay->AddWidget(std::move(widget));

        configuration->screen_configurations.push_back(std::move(overlay));
    }

    return configuration;
}

// The stored bytes of the UI configuration, as a client would read them.
std::vector<uint8_t> StoredBytes(IFsService& fs_service) {
    constexpr const char* PATH = "config/ui_config.cbor";

    const auto size = fs_service.GetFileSize(PATH);
    if(!size.has_value())
        return {};

    std::vector<uint8_t> bytes(*size);
    size_t read = 0;
    zassert_true(fs_service.ReadFile(PATH, bytes.data(), bytes.size(), read));
    bytes.resize(read);

    return bytes;
}

} // namespace

// A unit that stores a UI configuration keeps it: the demo is only for one that stores none.
ZTEST_SUITE(ui_controller_stored, NULL, NULL, NULL, NULL, NULL);

ZTEST(ui_controller_stored, test_a_stored_configuration_survives_initialize) {
    EnsureBus();
    auto fs_service = FormatFs();
    auto config_work_queue = StartConfigWorkQueue();

    // What an earlier boot, or a client, stored.
    {
        auto cbor_service = std::make_unique<CborConfigurationService<CborUiConfig>>(
            "ui_config", fs_service, config_work_queue
        );
        UiConfigurationManager manager(std::move(cbor_service));
        zassert_true(manager.Update(MakeConfiguration({ 7, 8 })));
    }
    const auto stored = StoredBytes(*fs_service);
    zassert_true(!stored.empty(), "Expected the configuration to be stored.");

    auto assets_manager = std::make_shared<AssetsManager>();
    auto controller = std::make_shared<UiController>(fs_service, config_work_queue, nullptr, assets_manager);
    zassert_equal(controller->Initialize(), 0, "Expected the UI controller to initialize.");

    zassert_true(StoredBytes(*fs_service) == stored, "Expected the stored bytes to be as they were.");

    const auto configuration = controller->GetConfigurationManager()->Get();
    zassert_equal(configuration->screen_configurations.size(), 2U);
    zassert_equal(configuration->active_screen_group_id, 7U);

    // The screens shown are the stored ones, not the demo's.
    const auto navigation = controller->GetNavigationService();
    zassert_true(navigation->GetActiveGroupId().has_value());
    zassert_equal(*navigation->GetActiveGroupId(), 7U);
    zassert_equal(navigation->GoToGroup(FIRST_GROUP_ID), -ENOENT, "Expected the demo's group to be absent.");

    // Nor are the demo's files put in its place.
    for(const auto kind : { AssetKind::IMAGES, AssetKind::ANIMATIONS }) {
        const auto provider = assets_manager->Find(kind);
        zassert_not_null(provider.get());
        for(const auto name : DEMO_ASSETS)
            zassert_false(provider->store->Exists(name), "Expected %s not to be written.", std::string(name).c_str());
    }
}

#ifdef CONFIG_EERIE_LEAP_UI_DEMO_CONFIGURATION
// A unit that stores nothing shows the demo, and what it stored is kept from then on.
ZTEST(ui_controller_stored, test_the_demo_is_stored_once_and_then_kept) {
    EnsureBus();
    auto fs_service = FormatFs();
    auto config_work_queue = StartConfigWorkQueue();

    zassert_false(fs_service->Exists("config/ui_config.cbor"));

    std::vector<uint8_t> demo;
    {
        auto controller = std::make_shared<UiController>(fs_service, config_work_queue, nullptr, nullptr);
        zassert_equal(controller->Initialize(), 0, "Expected the UI controller to initialize.");

        demo = StoredBytes(*fs_service);
        zassert_true(!demo.empty(), "Expected the demo to be stored.");
        zassert_equal(controller->GetConfigurationManager()->Get()->screen_configurations.size(), 4U);
    }

    // The next boot finds it and installs nothing.
    auto controller = std::make_shared<UiController>(fs_service, config_work_queue, nullptr, nullptr);
    zassert_equal(controller->Initialize(), 0, "Expected the UI controller to initialize.");
    zassert_true(StoredBytes(*fs_service) == demo);
}

// A file already there under a demo file's name is one a client may have put there.
ZTEST(ui_controller_stored, test_demo_assets_only_fill_in_missing_files) {
    EnsureBus();
    auto fs_service = FormatFs();
    auto config_work_queue = StartConfigWorkQueue();

    const std::array<uint8_t, 4> client_file{ 1, 2, 3, 4 };
    {
        AssetStore animations(fs_service, "ui_assets/animations");
        zassert_true(animations.Save("ui_lottie_spinner.json", client_file));
        AssetStore images(fs_service, "ui_assets/images");
        zassert_true(images.Save("ui_img_arrow_al88.bin", client_file));
    }

    auto assets_manager = std::make_shared<AssetsManager>();
    auto controller = std::make_shared<UiController>(fs_service, config_work_queue, nullptr, assets_manager);
    zassert_equal(controller->Initialize(), 0, "Expected the UI controller to initialize.");

    const auto animations = assets_manager->Find(AssetKind::ANIMATIONS);
    const auto images = assets_manager->Find(AssetKind::IMAGES);
    CheckLoads(*animations, "ui_lottie_spinner.json", client_file);
    CheckLoads(*images, "ui_img_arrow_al88.bin", client_file);
    // The one that was missing is filled in.
    CheckLoads(*images, "ui_img_norma_al88.bin", UI_IMG_NORMA_AL88_DATA);
}

#endif
#ifndef CONFIG_EERIE_LEAP_UI_DEMO_CONFIGURATION
// Without the demo a unit that stores nothing shows nothing and writes nothing.
ZTEST(ui_controller_stored, test_without_the_demo_a_unit_with_nothing_stored_shows_nothing) {
    EnsureBus();
    auto fs_service = FormatFs();
    auto config_work_queue = StartConfigWorkQueue();

    auto assets_manager = std::make_shared<AssetsManager>();
    auto controller = std::make_shared<UiController>(fs_service, config_work_queue, nullptr, assets_manager);
    zassert_equal(controller->Initialize(), 0, "Expected the UI controller to initialize.");

    zassert_true(controller->GetConfigurationManager()->Get()->screen_configurations.empty());
    zassert_false(controller->GetNavigationService()->GetActiveGroupId().has_value());
    zassert_false(controller->IsShowingScreens());

    for(const auto kind : { AssetKind::IMAGES, AssetKind::ANIMATIONS }) {
        const auto provider = assets_manager->Find(kind);
        zassert_not_null(provider.get());
        for(const auto name : DEMO_ASSETS)
            zassert_false(provider->store->Exists(name), "Expected %s not to be written.", std::string(name).c_str());
    }
}
#endif

namespace {

constexpr int REBUILD_TIMEOUT_MS = 5000;
constexpr int POLL_MS = 20;
constexpr uint32_t STORED_GROUP_ID = 5;
constexpr uint32_t SECOND_STORED_GROUP_ID = 6;

// A rebuild is done when the controller has replayed the state to the new screens: the last thing it does.
struct RebuildFixture {
    std::shared_ptr<FsService> fs_service;
    std::shared_ptr<WorkQueueThread> config_work_queue;
    std::shared_ptr<UiController> controller;
    std::shared_ptr<UiConfigurationManager> manager;
    std::shared_ptr<NavigationService> navigation_service;
    // What logging's stop hangs on: it is told of every stored configuration, and must not be told of a preview.
    std::shared_ptr<ConfigurationService> configuration_service;
    std::atomic<int> stored_count = 0;

    k_sem replayed{};
    std::atomic<int> replay_count = 0;
    k_sem settings_requested{};
    AnySubscription settings_subscription;
};

RebuildFixture rebuild;

void* RebuildSetup() {
    EnsureBus();

    rebuild.fs_service = FormatFs();
    rebuild.config_work_queue = StartConfigWorkQueue();

    rebuild.configuration_service = std::make_shared<ConfigurationService>();
    rebuild.configuration_service->RegisterConfigurationStoredHandler([](ConfigurationService::Type) {
        ++rebuild.stored_count;
    });
    rebuild.controller = std::make_shared<UiController>(
        rebuild.fs_service, rebuild.config_work_queue, rebuild.configuration_service, std::make_shared<AssetsManager>()
    );
    zassert_equal(rebuild.controller->Initialize(), 0, "Expected the UI controller to initialize.");
    // Without the demo there is nothing to show yet, and Start() says so; the first stored configuration must
    // still be shown.
    const int started = rebuild.controller->Start();
#ifdef CONFIG_EERIE_LEAP_UI_DEMO_CONFIGURATION
    zassert_equal(started, 0, "Expected the UI controller to start.");
#else
    zassert_not_equal(started, 0, "Expected nothing to show without the demo.");
#endif

    rebuild.manager = rebuild.controller->GetConfigurationManager();
    rebuild.navigation_service = rebuild.controller->GetNavigationService();

    k_sem_init(&rebuild.replayed, 0, K_SEM_MAX_LIMIT);
    k_sem_init(&rebuild.settings_requested, 0, K_SEM_MAX_LIMIT);

    rebuild.controller->SetStateReplay([] {
        ++rebuild.replay_count;
        k_sem_give(&rebuild.replayed);
    });

    rebuild.settings_subscription = CreateScopedSubscription(
        SettingsEventsChannel::GetInstance(), SettingsEventType::StateRequested,
        [](const SettingsEventsChannel::EventMessage&) {
            k_sem_give(&rebuild.settings_requested);
        }
    );
    zassert_not_null(rebuild.settings_subscription.get(), "Expected to subscribe to settings events.");

    return nullptr;
}

void RebuildBefore(void*) {
    if(rebuild.navigation_service->IsOverlayActive())
        rebuild.navigation_service->CloseOverlay();

    // Whatever a rebuild of the test before still sends has been sent once nothing arrives for a while.
    while(k_sem_take(&rebuild.replayed, K_MSEC(SETTLE_MS)) == 0) {
    }
    k_sem_reset(&rebuild.settings_requested);
    rebuild.replay_count = 0;
}

void RebuildTeardown(void*) {
    rebuild.configuration_service->RegisterConfigurationStoredHandler(nullptr);
    rebuild.settings_subscription.reset();
    rebuild.manager.reset();
    rebuild.navigation_service.reset();
    rebuild.controller.reset();
    rebuild.config_work_queue.reset();
    rebuild.fs_service.reset();
}

bool WaitForRebuild() {
    return k_sem_take(&rebuild.replayed, K_MSEC(REBUILD_TIMEOUT_MS)) == 0;
}

} // namespace

ZTEST_SUITE(ui_controller_rebuild, NULL, RebuildSetup, RebuildBefore, NULL, RebuildTeardown);

// The point of the handler: a stored configuration reaches the screen, and the owners are asked for what
// the new widgets' bindings missed.
ZTEST(ui_controller_rebuild, test_a_stored_configuration_rebuilds_the_screens_and_requests_the_state) {
    zassert_true(rebuild.manager->Update(MakeConfiguration({ STORED_GROUP_ID, SECOND_STORED_GROUP_ID })));

    zassert_true(WaitForRebuild(), "Expected the stored configuration to be built.");
    zassert_equal(
        k_sem_take(&rebuild.settings_requested, K_MSEC(DISPATCH_TIMEOUT_MS)), 0,
        "Expected the settings to be requested."
    );
    zassert_equal(rebuild.replay_count.load(), 1);

    zassert_true(rebuild.navigation_service->GetActiveGroupId().has_value());
    zassert_equal(*rebuild.navigation_service->GetActiveGroupId(), STORED_GROUP_ID);
    zassert_equal(rebuild.navigation_service->GoToGroup(FIRST_GROUP_ID), -ENOENT, "Expected the demo's groups gone.");
    zassert_equal(rebuild.navigation_service->GoToGroup(1), -ENOENT, "Expected the demo's groups gone.");

    // The new groups are navigable, from the new start group.
    zassert_equal(rebuild.navigation_service->Next(), 0);
    k_msleep(SETTLE_MS);
    zassert_equal(*rebuild.navigation_service->GetActiveGroupId(), SECOND_STORED_GROUP_ID);
}

// An overlay of the old screens would otherwise sit over the new ones, and block navigation.
ZTEST(ui_controller_rebuild, test_a_rebuild_dismisses_an_open_overlay) {
    constexpr uint32_t OVERLAY_SCREEN_ID = 40;

    zassert_true(
        rebuild.manager->Update(MakeConfiguration({ STORED_GROUP_ID, SECOND_STORED_GROUP_ID }, OVERLAY_SCREEN_ID))
    );
    zassert_true(WaitForRebuild());

    zassert_equal(rebuild.navigation_service->ShowOverlay(OVERLAY_SCREEN_ID), 0);
    k_msleep(SETTLE_MS);
    zassert_true(rebuild.navigation_service->IsOverlayActive(), "Expected the overlay to be up.");
    zassert_equal(rebuild.navigation_service->Next(), -EBUSY);

    zassert_true(rebuild.manager->Update(MakeConfiguration({ SECOND_STORED_GROUP_ID, STORED_GROUP_ID })));
    zassert_true(WaitForRebuild());

    zassert_false(rebuild.navigation_service->IsOverlayActive());
    zassert_equal(*rebuild.navigation_service->GetActiveGroupId(), SECOND_STORED_GROUP_ID);
    zassert_equal(rebuild.navigation_service->Next(), 0, "Expected navigation to work on the new screens.");
    k_msleep(SETTLE_MS);
    zassert_equal(*rebuild.navigation_service->GetActiveGroupId(), STORED_GROUP_ID);
}

// A configuration a client uploads is stored as the bytes it sent, and shown the same way.
ZTEST(ui_controller_rebuild, test_a_client_apply_rebuilds_the_screens) {
    zassert_true(rebuild.manager->Update(MakeConfiguration({ STORED_GROUP_ID })));
    zassert_true(WaitForRebuild());
    const auto uploaded = rebuild.manager->GetCborConfiguration();
    zassert_true(!uploaded.empty());

    zassert_true(rebuild.manager->Update(MakeConfiguration({ SECOND_STORED_GROUP_ID })));
    zassert_true(WaitForRebuild());
    zassert_equal(*rebuild.navigation_service->GetActiveGroupId(), SECOND_STORED_GROUP_ID);

    std::array<char, 128> reason{};
    zassert_true(rebuild.manager->ApplyCborConfiguration(uploaded, reason), "Refused: %s", reason.data());

    zassert_true(WaitForRebuild(), "Expected the applied configuration to be built.");
    zassert_equal(*rebuild.navigation_service->GetActiveGroupId(), STORED_GROUP_ID);
    zassert_equal(rebuild.navigation_service->GoToGroup(SECOND_STORED_GROUP_ID), -ENOENT);
}

// A configuration the unit refuses is stored by no one, so it is shown by no one.
ZTEST(ui_controller_rebuild, test_a_refused_apply_leaves_the_screens_alone) {
    zassert_true(rebuild.manager->Update(MakeConfiguration({ STORED_GROUP_ID })));
    zassert_true(WaitForRebuild());

    std::array<char, 128> reason{};
    const std::array<uint8_t, 3> garbage{ 0x01, 0x02, 0x03 };
    zassert_false(rebuild.manager->ApplyCborConfiguration(garbage, reason));

    zassert_not_equal(k_sem_take(&rebuild.replayed, K_MSEC(SETTLE_MS)), 0, "Expected no rebuild.");
    zassert_equal(*rebuild.navigation_service->GetActiveGroupId(), STORED_GROUP_ID);
}

// Stores that follow each other leave the last one on screen, however many rebuilds that takes.
ZTEST(ui_controller_rebuild, test_stores_in_a_row_leave_the_last_one_shown) {
    constexpr uint32_t LAST_GROUP_ID = 13;

    for(const uint32_t group_id : { 11U, 12U, LAST_GROUP_ID })
        zassert_true(rebuild.manager->Update(MakeConfiguration({ group_id })));

    // However many rebuilds the stores made, the last one is the group shown once they are done.
    const int64_t deadline = k_uptime_get() + REBUILD_TIMEOUT_MS;
    while(rebuild.navigation_service->GetActiveGroupId() != LAST_GROUP_ID && k_uptime_get() < deadline)
        k_msleep(POLL_MS);

    zassert_true(rebuild.navigation_service->GetActiveGroupId() == LAST_GROUP_ID, "Expected the last store shown.");
    zassert_true(WaitForRebuild());
    zassert_true(rebuild.replay_count.load() >= 1);
}

// A stored configuration with no screens shows nothing, and the next one still builds.
ZTEST(ui_controller_rebuild, test_a_configuration_without_screens_leaves_no_groups) {
    auto empty = eerie_memory::make_shared_pmr<UiConfiguration>(Mrm::GetExtPmr());

    zassert_true(rebuild.manager->Update(std::move(empty)));
    zassert_true(WaitForRebuild());

    zassert_false(rebuild.navigation_service->GetActiveGroupId().has_value());
    zassert_equal(rebuild.navigation_service->GoToGroup(FIRST_GROUP_ID), -ENOENT);

    // And a configuration after it builds as usual.
    zassert_true(rebuild.manager->Update(MakeConfiguration({ STORED_GROUP_ID })));
    zassert_true(WaitForRebuild());
    zassert_equal(*rebuild.navigation_service->GetActiveGroupId(), STORED_GROUP_ID);
}

// The first thing stored on a unit that showed nothing (a release build, without the demo) must reach the screen too:
// the view was never rendered, so it renders now.
ZTEST(ui_controller_rebuild, test_the_first_stored_configuration_is_shown) {
    zassert_true(rebuild.manager->Update(MakeConfiguration({ STORED_GROUP_ID })));
    zassert_true(WaitForRebuild());

    zassert_true(rebuild.controller->IsShowingScreens());
    zassert_equal(*rebuild.navigation_service->GetActiveGroupId(), STORED_GROUP_ID);
}

// An event is delivered some time after it was published. One for the screens of a configuration that has since
// been replaced would show an overlay that navigation no longer holds, or a group that was not asked for.
ZTEST(ui_controller_rebuild, test_a_stale_navigation_event_is_dropped) {
    constexpr uint32_t OVERLAY_SCREEN_ID = 40;

    zassert_true(
        rebuild.manager->Update(MakeConfiguration({ STORED_GROUP_ID, SECOND_STORED_GROUP_ID }, OVERLAY_SCREEN_ID))
    );
    zassert_true(WaitForRebuild());
    k_sem_reset(&rebuild.settings_requested);

    // The navigation holds no overlay, so an event to show one is from before the rebuild.
    NavigationEventChannel::GetInstance().Publish(
        { .source_id = 0,
            .type = NavigationEventType::Changed,
            .payload = { { NavigationPayloadType::Action, static_cast<uint32_t>(NavigationAction::ShowOverlay) },
                { NavigationPayloadType::TargetScreenId, OVERLAY_SCREEN_ID } } }
    );
    zassert_not_equal(
        k_sem_take(&rebuild.settings_requested, K_MSEC(SETTLE_MS)), 0, "Expected the stale overlay not to be built."
    );
    zassert_false(rebuild.navigation_service->IsOverlayActive());

    // And a group that is not the one asked for last.
    NavigationEventChannel::GetInstance().Publish(
        { .source_id = 0,
            .type = NavigationEventType::Changed,
            .payload = { { NavigationPayloadType::Action, static_cast<uint32_t>(NavigationAction::ShowGroup) },
                { NavigationPayloadType::TargetGroupId, SECOND_STORED_GROUP_ID } } }
    );
    k_msleep(SETTLE_MS);
    zassert_equal(*rebuild.navigation_service->GetActiveGroupId(), STORED_GROUP_ID);

    // The ones navigation asked for are still shown.
    zassert_equal(rebuild.navigation_service->ShowOverlay(OVERLAY_SCREEN_ID), 0);
    zassert_equal(k_sem_take(&rebuild.settings_requested, K_MSEC(DISPATCH_TIMEOUT_MS)), 0);
    zassert_true(rebuild.navigation_service->IsOverlayActive());
}

// ======== The UI preview ========

namespace {

// How long the tests give a lease of CONFIG_EERIE_LEAP_UI_PREVIEW_LEASE_MS to run out.
constexpr int LEASE_MS = CONFIG_EERIE_LEAP_UI_PREVIEW_LEASE_MS;
constexpr int OVERLAY_SCREEN_ID = 40;
constexpr uint32_t PREVIEW_GROUP_ID = 11;
constexpr uint32_t SECOND_PREVIEW_GROUP_ID = 12;
constexpr size_t STAGE_CHUNK_SIZE = 400;

UiPreviewService& Preview() {
    const auto service = rebuild.controller->GetPreviewService();
    zassert_not_null(service.get(), "Expected the controller to serve a preview.");

    return *service;
}

std::vector<uint8_t> Encode(const std::shared_ptr<UiConfiguration>& configuration) {
    const auto cbor_configuration = UiConfigurationCborParser().Serialize(*configuration);
    const auto bytes = CborSerializer<CborUiConfig>().Serialize(*cbor_configuration);

    return { bytes.begin(), bytes.end() };
}

uint32_t Crc(std::span<const uint8_t> bytes) {
    return crc32_ieee(bytes.data(), bytes.size());
}

UiPreviewError StageBytes(
    uint32_t token,
    std::span<const uint8_t> bytes,
    UiPreviewService::StageInfo& info,
    std::optional<uint32_t> rev = std::nullopt,
    std::optional<uint32_t> crc = std::nullopt
) {

    for(size_t offset = 0; offset < bytes.size(); offset += STAGE_CHUNK_SIZE) {
        const size_t size = std::min(STAGE_CHUNK_SIZE, bytes.size() - offset);
        const bool is_first = offset == 0;

        const UiPreviewService::StageRequest request{ .token = token,
            .offset = static_cast<uint32_t>(offset),
            .data = bytes.subspan(offset, size),
            .length = is_first ? std::optional<uint32_t>(bytes.size()) : std::nullopt,
            .crc = is_first ? std::optional<uint32_t>(crc.value_or(Crc(bytes))) : std::nullopt,
            .rev = is_first ? rev : std::nullopt };

        if(const auto error = Preview().Stage(request, info); error != UiPreviewError::OK)
            return error;
    }

    return UiPreviewError::OK;
}

UiPreviewError Stage(uint32_t token, const std::shared_ptr<UiConfiguration>& configuration) {
    UiPreviewService::StageInfo info;

    return StageBytes(token, Encode(configuration), info);
}

uint32_t BeginPreview(bool is_pinned = true) {
    UiPreviewService::BeginInfo info;
    zassert_equal(Preview().Begin(std::nullopt, is_pinned, info), UiPreviewError::OK);
    zassert_not_equal(info.token, 0U);

    return info.token;
}

bool WaitUntilGroupIs(uint32_t group_id) {
    const int64_t deadline = k_uptime_get() + REBUILD_TIMEOUT_MS;
    while(rebuild.navigation_service->GetActiveGroupId() != group_id && k_uptime_get() < deadline)
        k_msleep(POLL_MS);

    return rebuild.navigation_service->GetActiveGroupId() == group_id;
}

bool WaitUntilPreviewEnds() {
    const int64_t deadline = k_uptime_get() + LEASE_MS + REBUILD_TIMEOUT_MS;
    while(Preview().IsActive() && k_uptime_get() < deadline)
        k_msleep(POLL_MS);

    return !Preview().IsActive();
}

// A test that fails with a session open must not leave it for the next.
void PreviewAfter(void*) {
    const auto service = rebuild.controller->GetPreviewService();
    if(service != nullptr && service->IsActive()) {
        service->OnLinkDropped();
        zassert_true(WaitUntilPreviewEnds());
    }
}

// The outline over the screens: the last object of the top layer, if shown.
lv_obj_t* ShownOutline() {
    ScopedLvglLock lvgl_guard;

    lv_obj_t* layer = lv_layer_top();
    const auto count = lv_obj_get_child_count(layer);
    if(count == 0)
        return nullptr;

    lv_obj_t* outline = lv_obj_get_child(layer, static_cast<int32_t>(count) - 1);

    return lv_obj_is_hidden(outline) ? nullptr : outline;
}

void StoreAndShow(std::initializer_list<uint32_t> group_ids, std::optional<uint32_t> overlay = std::nullopt) {
    zassert_true(rebuild.manager->Update(MakeConfiguration(group_ids, overlay)));
    zassert_true(WaitForRebuild());
    zassert_equal(*rebuild.navigation_service->GetActiveGroupId(), *group_ids.begin());
}

} // namespace

ZTEST_SUITE(ui_controller_preview, NULL, RebuildSetup, RebuildBefore, PreviewAfter, RebuildTeardown);

// The point of the preview: the screens change, the unit stores nothing and tells nothing that stores, and
// the end puts the stored screens back.
ZTEST(ui_controller_preview, test_a_staged_configuration_is_shown_and_the_end_puts_the_stored_screens_back) {
    StoreAndShow({ STORED_GROUP_ID, SECOND_STORED_GROUP_ID });
    const auto stored_bytes = rebuild.manager->GetCborConfiguration();
    const auto stored_info = rebuild.manager->GetCborConfigurationInfo();
    const int stored_count = rebuild.stored_count.load();

    UiPreviewService::BeginInfo begin;
    zassert_equal(Preview().Begin(std::nullopt, true, begin), UiPreviewError::OK);
    zassert_not_equal(begin.token, 0U);
    zassert_equal(begin.lease_ms, static_cast<uint32_t>(LEASE_MS));
    zassert_equal(begin.rev, 0U);
    zassert_equal(begin.stored_crc, stored_info.crc, "BEGIN says what the unit stores");
    zassert_equal(begin.max_stage, static_cast<uint32_t>(CONFIG_EERIE_LEAP_UI_PREVIEW_MAX_STAGE_SIZE));
    zassert_equal(begin.max_patch, 0U, "The unit takes no patch yet");
    zassert_true(Preview().IsActive());

    const auto staged = MakeConfiguration({ PREVIEW_GROUP_ID, SECOND_PREVIEW_GROUP_ID });
    UiPreviewService::StageInfo stage;
    zassert_equal(StageBytes(begin.token, Encode(staged), stage), UiPreviewError::OK);
    zassert_true(stage.rev.has_value());
    zassert_equal(*stage.rev, 1U);

    zassert_equal(*rebuild.navigation_service->GetActiveGroupId(), PREVIEW_GROUP_ID);
    zassert_equal(rebuild.navigation_service->GoToGroup(STORED_GROUP_ID), -ENOENT, "The stored groups are not shown");
    zassert_true(rebuild.controller->IsShowingScreens());

    // Nothing was stored, and nothing that reacts to a store was told.
    zassert_true(rebuild.manager->GetCborConfiguration() == stored_bytes);
    zassert_equal(rebuild.manager->GetCborConfigurationInfo().crc, stored_info.crc);
    zassert_equal(rebuild.stored_count.load(), stored_count, "No stored handler is called by a preview");

    bool is_reverted = false;
    zassert_equal(Preview().End(begin.token, is_reverted), UiPreviewError::OK);
    zassert_true(is_reverted);
    zassert_false(Preview().IsActive());

    zassert_equal(*rebuild.navigation_service->GetActiveGroupId(), STORED_GROUP_ID);
    zassert_equal(rebuild.navigation_service->GoToGroup(PREVIEW_GROUP_ID), -ENOENT, "The preview's groups are gone");
    zassert_true(rebuild.manager->GetCborConfiguration() == stored_bytes);
    zassert_equal(rebuild.stored_count.load(), stored_count);
}

ZTEST(ui_controller_preview, test_ending_a_preview_that_showed_nothing_reverts_nothing) {
    StoreAndShow({ STORED_GROUP_ID });
    const auto token = BeginPreview();
    k_sem_reset(&rebuild.replayed);

    bool is_reverted = true;
    zassert_equal(Preview().End(token, is_reverted), UiPreviewError::OK);

    zassert_false(is_reverted);
    zassert_not_equal(k_sem_take(&rebuild.replayed, K_MSEC(SETTLE_MS)), 0, "The screens were not rebuilt");
}

// The client that goes away without ending must not leave the unit on a screen nobody stores.
ZTEST(ui_controller_preview, test_the_lease_running_out_puts_the_stored_screens_back) {
    StoreAndShow({ STORED_GROUP_ID });
    const int stored_count = rebuild.stored_count.load();
    const auto token = BeginPreview();
    zassert_equal(Stage(token, MakeConfiguration({ PREVIEW_GROUP_ID })), UiPreviewError::OK);
    zassert_equal(*rebuild.navigation_service->GetActiveGroupId(), PREVIEW_GROUP_ID);

    zassert_true(WaitUntilPreviewEnds(), "Expected the lease to end the session.");

    zassert_true(WaitUntilGroupIs(STORED_GROUP_ID));
    zassert_equal(rebuild.stored_count.load(), stored_count);
    zassert_true(rebuild.controller->IsNavigationEnabled(), "and the pin is released");

    // The token is of no use any more.
    bool is_reverted = false;
    zassert_equal(Preview().End(token, is_reverted), UiPreviewError::NO_SESSION);
}

ZTEST(ui_controller_preview, test_a_request_that_carries_the_token_renews_the_lease) {
    StoreAndShow({ STORED_GROUP_ID });
    const auto token = BeginPreview();

    // Together longer than the lease.
    for(int i = 0; i < 6; ++i) {
        k_msleep(LEASE_MS / 3);
        zassert_equal(Preview().Highlight(token, std::nullopt, std::nullopt), UiPreviewError::OK);
        zassert_true(Preview().IsActive(), "Round %d", i);
    }

    const auto status = Preview().GetStatus();
    zassert_true(status.is_active);
    zassert_true(status.lease_left_ms > 0 && status.lease_left_ms <= static_cast<uint32_t>(LEASE_MS));
}

// What the client uploads is stored, and it is what shows; the preview has nothing left to put back.
ZTEST(ui_controller_preview, test_a_stored_configuration_ends_the_preview_and_shows_what_it_stored) {
    StoreAndShow({ STORED_GROUP_ID });
    const auto token = BeginPreview();
    zassert_equal(Stage(token, MakeConfiguration({ PREVIEW_GROUP_ID })), UiPreviewError::OK);

    constexpr uint32_t UPLOADED_GROUP_ID = 20;
    zassert_true(rebuild.manager->Update(MakeConfiguration({ UPLOADED_GROUP_ID })));

    zassert_true(WaitUntilPreviewEnds());
    zassert_true(WaitUntilGroupIs(UPLOADED_GROUP_ID));
    k_msleep(SETTLE_MS);
    zassert_equal(*rebuild.navigation_service->GetActiveGroupId(), UPLOADED_GROUP_ID, "The store is what shows");
    zassert_equal(rebuild.navigation_service->GoToGroup(STORED_GROUP_ID), -ENOENT);
    zassert_true(rebuild.controller->IsNavigationEnabled());

    bool is_reverted = false;
    zassert_equal(Preview().End(token, is_reverted), UiPreviewError::NO_SESSION);
}

ZTEST(ui_controller_preview, test_the_local_link_dropping_ends_the_preview_and_puts_the_stored_screens_back) {
    StoreAndShow({ STORED_GROUP_ID });
    const auto token = BeginPreview();
    zassert_equal(Stage(token, MakeConfiguration({ PREVIEW_GROUP_ID })), UiPreviewError::OK);

    Preview().OnLinkDropped();

    zassert_true(WaitUntilPreviewEnds());
    zassert_true(WaitUntilGroupIs(STORED_GROUP_ID));
}

ZTEST(ui_controller_preview, test_the_pin_keeps_the_swipes_from_navigating_while_the_preview_lives) {
    StoreAndShow({ STORED_GROUP_ID });
    zassert_true(rebuild.controller->IsNavigationEnabled());

    auto token = BeginPreview(true);
    zassert_false(rebuild.controller->IsNavigationEnabled());

    bool is_reverted = false;
    zassert_equal(Preview().End(token, is_reverted), UiPreviewError::OK);
    zassert_true(rebuild.controller->IsNavigationEnabled());

    BeginPreview(false);
    zassert_true(rebuild.controller->IsNavigationEnabled(), "A preview that does not pin leaves the swipes alone");
}

ZTEST(ui_controller_preview, test_a_second_client_is_refused_and_a_repeated_begin_gets_the_same_token) {
    StoreAndShow({ STORED_GROUP_ID });
    UiPreviewService::BeginInfo first;
    zassert_equal(Preview().Begin(std::nullopt, true, first), UiPreviewError::OK);

    // The answer got lost, and the client asks again.
    UiPreviewService::BeginInfo repeated;
    zassert_equal(Preview().Begin(std::nullopt, true, repeated), UiPreviewError::OK);
    zassert_equal(repeated.token, first.token);

    // With the token it renews.
    UiPreviewService::BeginInfo renewed;
    zassert_equal(Preview().Begin(first.token, true, renewed), UiPreviewError::OK);
    zassert_equal(renewed.token, first.token);

    // Someone else, or the client after its token was used: busy.
    UiPreviewService::BeginInfo other;
    zassert_equal(Preview().Begin(std::nullopt, true, other), UiPreviewError::BUSY);
    zassert_equal(Preview().Begin(first.token + 1, true, other), UiPreviewError::BAD_TOKEN);
}

ZTEST(ui_controller_preview, test_requests_without_the_session_are_refused) {
    StoreAndShow({ STORED_GROUP_ID });
    const UiPreviewTarget group{ .is_overlay = false, .id = STORED_GROUP_ID };
    UiPreviewShown shown;
    bool is_reverted = false;
    UiPreviewService::StageInfo info;

    zassert_equal(Preview().Show(1, group, shown), UiPreviewError::NO_SESSION);
    zassert_equal(Preview().Highlight(1, std::nullopt, std::nullopt), UiPreviewError::NO_SESSION);
    zassert_equal(Preview().End(1, is_reverted), UiPreviewError::NO_SESSION);
    zassert_equal(StageBytes(1, Encode(MakeConfiguration({ PREVIEW_GROUP_ID })), info), UiPreviewError::NO_SESSION);

    const auto token = BeginPreview();
    zassert_equal(Preview().Show(token + 1, group, shown), UiPreviewError::BAD_TOKEN);
    zassert_equal(Preview().Highlight(token + 1, std::nullopt, std::nullopt), UiPreviewError::BAD_TOKEN);
    zassert_equal(Preview().End(token + 1, is_reverted), UiPreviewError::BAD_TOKEN);
    zassert_true(Preview().IsActive(), "A wrong token ends nothing");
}

ZTEST(ui_controller_preview, test_a_staged_configuration_is_checked_as_an_upload_is) {
    StoreAndShow({ STORED_GROUP_ID });
    const auto token = BeginPreview();
    const auto good = Encode(MakeConfiguration({ PREVIEW_GROUP_ID }));
    UiPreviewService::StageInfo info;

    // A CRC the bytes do not have.
    zassert_equal(StageBytes(token, good, info, std::nullopt, Crc(good) ^ 1U), UiPreviewError::CRC_MISMATCH);

    // Not CBOR of a configuration.
    const std::vector<uint8_t> garbage(40, 0xFF);
    zassert_equal(StageBytes(token, garbage, info), UiPreviewError::INVALID);
    zassert_true(Preview().GetReason() == "Malformed CBOR");

    // CBOR the validator refuses: two screens with one ID (the second screen's [id 1, group 12, Gauge, z 0, ...]).
    auto invalid = Encode(MakeConfiguration({ PREVIEW_GROUP_ID, SECOND_PREVIEW_GROUP_ID }));
    constexpr std::array<uint8_t, 7> SECOND_SCREEN = { 0x9f, 0x01, SECOND_PREVIEW_GROUP_ID, 0x02, 0x00, 0xf5, 0xf4 };
    const auto second_screen = std::ranges::search(invalid, SECOND_SCREEN);
    zassert_false(second_screen.empty(), "Expected the second screen's record in the bytes.");
    *(second_screen.begin() + 1) = 0x00;
    zassert_equal(StageBytes(token, invalid, info), UiPreviewError::INVALID);
    zassert_true(
        Preview().GetReason().contains("duplicate screen IDs"), "'%s'", std::string(Preview().GetReason()).c_str()
    );

    // Too large for the stage buffer.
    const UiPreviewService::StageRequest too_large{ .token = token,
        .offset = 0,
        .data = std::span(good).first(1),
        .length = static_cast<uint32_t>(CONFIG_EERIE_LEAP_UI_PREVIEW_MAX_STAGE_SIZE) + 1,
        .crc = 0 };
    zassert_equal(Preview().Stage(too_large, info), UiPreviewError::TOO_LARGE);

    // Out of order.
    const UiPreviewService::StageRequest skipping{ .token = token, .offset = 100, .data = std::span(good).first(1) };
    zassert_equal(Preview().Stage(skipping, info), UiPreviewError::BAD_OFFSET);
    zassert_equal(info.received, 0U);

    // None of it showed: the stored screens are still on, and the session lives to try again.
    zassert_equal(*rebuild.navigation_service->GetActiveGroupId(), STORED_GROUP_ID);
    zassert_true(Preview().IsActive());
    zassert_equal(StageBytes(token, good, info), UiPreviewError::OK);
    zassert_equal(*rebuild.navigation_service->GetActiveGroupId(), PREVIEW_GROUP_ID);
}

// A revision the client gives must go forward, else its older staging would replace a newer.
ZTEST(ui_controller_preview, test_the_revision_goes_forward) {
    StoreAndShow({ STORED_GROUP_ID });
    const auto token = BeginPreview();
    const auto bytes = Encode(MakeConfiguration({ PREVIEW_GROUP_ID }));
    UiPreviewService::StageInfo info;

    zassert_equal(StageBytes(token, bytes, info, 5), UiPreviewError::OK);
    zassert_equal(*info.rev, 5U);

    zassert_equal(StageBytes(token, bytes, info, 5), UiPreviewError::STALE);
    zassert_equal(info.current_rev, 5U);
    zassert_equal(StageBytes(token, bytes, info, 3), UiPreviewError::STALE);

    zassert_equal(StageBytes(token, bytes, info), UiPreviewError::OK);
    zassert_equal(*info.rev, 6U, "Without one the unit counts on");

    const auto status = Preview().GetStatus();
    zassert_true(status.is_active);
    zassert_equal(status.rev, 6U);
}

ZTEST(ui_controller_preview, test_show_asks_navigation_for_a_group_or_an_overlay) {
    StoreAndShow({ STORED_GROUP_ID });
    const auto token = BeginPreview();
    zassert_equal(
        Stage(token, MakeConfiguration({ PREVIEW_GROUP_ID, SECOND_PREVIEW_GROUP_ID }, OVERLAY_SCREEN_ID)),
        UiPreviewError::OK
    );

    UiPreviewShown shown;
    zassert_equal(
        Preview().Show(token, { .is_overlay = false, .id = SECOND_PREVIEW_GROUP_ID }, shown), UiPreviewError::OK
    );
    zassert_equal(shown.group, SECOND_PREVIEW_GROUP_ID);
    zassert_false(shown.overlay.has_value());
    zassert_true(WaitUntilGroupIs(SECOND_PREVIEW_GROUP_ID));

    // An overlay over the group shown.
    zassert_equal(Preview().Show(token, { .is_overlay = true, .id = OVERLAY_SCREEN_ID }, shown), UiPreviewError::OK);
    zassert_equal(shown.group, SECOND_PREVIEW_GROUP_ID);
    zassert_true(shown.overlay.has_value());
    zassert_equal(*shown.overlay, static_cast<uint32_t>(OVERLAY_SCREEN_ID));
    k_msleep(SETTLE_MS);
    zassert_true(rebuild.navigation_service->IsOverlayActive());
    zassert_true(rebuild.navigation_service->GetActiveOverlayId() == OVERLAY_SCREEN_ID);

    // A group closes the overlay first.
    zassert_equal(Preview().Show(token, { .is_overlay = false, .id = PREVIEW_GROUP_ID }, shown), UiPreviewError::OK);
    zassert_true(WaitUntilGroupIs(PREVIEW_GROUP_ID));
    k_msleep(SETTLE_MS);
    zassert_false(rebuild.navigation_service->IsOverlayActive());

    // What the configuration shown does not have.
    zassert_equal(Preview().Show(token, { .is_overlay = false, .id = 99 }, shown), UiPreviewError::NOT_FOUND);
    zassert_equal(Preview().Show(token, { .is_overlay = true, .id = 99 }, shown), UiPreviewError::NOT_FOUND);
    zassert_equal(
        Preview().Show(token, { .is_overlay = true, .id = 0 }, shown), UiPreviewError::NOT_FOUND,
        "A screen is no overlay"
    );
}

ZTEST(ui_controller_preview, test_highlight_outlines_a_widget_and_follows_it) {
    StoreAndShow({ STORED_GROUP_ID });
    const auto token = BeginPreview();
    zassert_equal(Stage(token, MakeConfiguration({ PREVIEW_GROUP_ID })), UiPreviewError::OK);
    zassert_is_null(ShownOutline(), "Nothing is outlined to begin with");

    // Screen 0 holds the bar, 100 by 20 at the bottom left.
    zassert_equal(Preview().Highlight(token, 0, 0), UiPreviewError::OK);
    lv_obj_t* outline = ShownOutline();
    zassert_not_null(outline, "Expected an outline.");
    {
        ScopedLvglLock lvgl_guard;
        lv_obj_update_layout(outline);
        zassert_equal(lv_obj_get_width(outline), 100 + 2 * 2);
        zassert_equal(lv_obj_get_height(outline), 20 + 2 * 2);
        zassert_equal(lv_obj_get_x(outline), -2);
        zassert_false(lv_obj_is_clickable(outline), "A touch goes through it");
    }

    // The widget alone is found among the screens shown.
    zassert_equal(Preview().Highlight(token, std::nullopt, 0), UiPreviewError::OK);
    zassert_not_null(ShownOutline());

    // It follows the screens through a rebuild, as the widget comes back under its IDs.
    zassert_equal(Stage(token, MakeConfiguration({ PREVIEW_GROUP_ID })), UiPreviewError::OK);
    k_msleep(SETTLE_MS);
    zassert_not_null(ShownOutline(), "Expected the outline to follow the new widget.");

    zassert_equal(Preview().Highlight(token, 0, 99), UiPreviewError::NOT_FOUND);
    zassert_equal(Preview().Highlight(token, 99, std::nullopt), UiPreviewError::NOT_FOUND);

    // Neither clears it, and so does the end.
    zassert_equal(Preview().Highlight(token, std::nullopt, std::nullopt), UiPreviewError::OK);
    zassert_is_null(ShownOutline());

    zassert_equal(Preview().Highlight(token, 0, 0), UiPreviewError::OK);
    zassert_not_null(ShownOutline());
    bool is_reverted = false;
    zassert_equal(Preview().End(token, is_reverted), UiPreviewError::OK);
    zassert_is_null(ShownOutline());
}

// Every command of a preview, and none of them stored a thing.
ZTEST(ui_controller_preview, test_no_command_of_a_preview_stores_or_reports_a_store) {
    StoreAndShow({ STORED_GROUP_ID });
    const int stored_count = rebuild.stored_count.load();
    const auto stored_info = rebuild.manager->GetCborConfigurationInfo();

    const auto token = BeginPreview();
    zassert_equal(Stage(token, MakeConfiguration({ PREVIEW_GROUP_ID }, OVERLAY_SCREEN_ID)), UiPreviewError::OK);
    UiPreviewShown shown;
    zassert_equal(Preview().Show(token, { .is_overlay = true, .id = OVERLAY_SCREEN_ID }, shown), UiPreviewError::OK);
    zassert_equal(Preview().Highlight(token, 0, 0), UiPreviewError::OK);
    bool is_reverted = false;
    zassert_equal(Preview().End(token, is_reverted), UiPreviewError::OK);

    zassert_equal(rebuild.stored_count.load(), stored_count);
    zassert_equal(rebuild.manager->GetCborConfigurationInfo().crc, stored_info.crc);
    zassert_equal(rebuild.manager->GetCborConfigurationInfo().size, stored_info.size);

    // The spy hears of a real store.
    zassert_true(rebuild.manager->Update(MakeConfiguration({ STORED_GROUP_ID })));
    zassert_equal(rebuild.stored_count.load(), stored_count + 1);
}
