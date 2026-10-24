#include <algorithm>
#include <cerrno>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

#include <eerie_memory.hpp>
#include <zephyr/logging/log.h>

#include "utilities/memory/memory_resource_manager.h"
#include "utilities/reflection/caller_name.h"

#include "configuration/cbor/cbor_ui_config/cbor_ui_config.h"
#include "configuration/services/cbor_configuration_service.h"

#include "domain/logging_domain/event_bus/logging_event_type.h"
#include "domain/logging_domain/event_bus/logging_payload_type.h"
#include "domain/sensor_domain/event_bus/sensor_event_type.h"
#include "domain/sensor_domain/event_bus/sensor_payload_type.h"
#include "domain/settings_domain/event_bus/settings_event_type.h"
#include "domain/settings_domain/event_bus/settings_events_channel.h"
#include "domain/settings_domain/event_bus/settings_payload_type.h"
#include "domain/settings_domain/models/setting_id.h"
#include "domain/ui_domain/event_bus/navigation_event_channel.h"
#include "domain/ui_domain/lvgl_lock.h"
#include "domain/ui_domain/models/animation.h"
#include "domain/ui_domain/models/icon_type.h"
#include "domain/ui_domain/models/navigation_intent.h"
#include "domain/ui_domain/models/property_binding.h"
#include "domain/ui_domain/models/widget_configuration.h"
#include "domain/ui_domain/models/widget_direction.h"
#include "domain/ui_domain/models/widget_fill_mode.h"
#include "domain/ui_domain/models/widget_property.h"
#include "domain/ui_domain/models/widget_type.h"

#include "subsys/assets/asset_kind.h"
#include "subsys/assets/asset_provider.h"
#include "subsys/event_bus/event_binding_subscription.h"
#include "subsys/event_bus/event_channel_id.h"
#include "subsys/threading/scoped_mutex.h"

#include "views/screens/i_screen.h"
#include "views/screens/screen_factory.h"
#include "views/themes/dark_bw_theme.h"
#include "views/themes/theme_manager.h"
#include "views/utilitites/frame.h"
#include "views/widgets/indicators/horizontal_chart_indicator/horizontal_chart_indicator.h"
#include "views/widgets/widget_factory.h"

#include "ui_controller.h"

namespace eerie_leap::controllers {

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::screens;
using namespace eerie_leap::views::themes;
using namespace eerie_leap::views::widgets::indicators;

namespace config_services = eerie_leap::configuration::services;

using eerie_leap::domain::settings_domain::event_bus::SettingsEventsChannel;
using eerie_leap::domain::settings_domain::event_bus::SettingsEventType;
using eerie_leap::domain::settings_domain::models::SettingId;
using eerie_leap::domain::ui_domain::ScopedLvglLock;
using eerie_leap::domain::ui_domain::event_bus::NavigationEventType;
using eerie_leap::domain::ui_domain::event_bus::NavigationPayloadType;
using eerie_leap::domain::ui_domain::models::Animation;
using eerie_leap::domain::ui_domain::preview::PatchEffect;
using eerie_leap::domain::ui_domain::preview::ScreenAction;
using eerie_leap::subsys::assets::AssetAccess;
using eerie_leap::subsys::assets::AssetKind;
using eerie_leap::subsys::assets::AssetProvider;
using eerie_leap::subsys::event_bus::CreateScopedSubscription;
using eerie_leap::subsys::event_bus::GetEventBindingError;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::utilities::reflection::GetCallerName;
using eerie_leap::views::widgets::WidgetFactory;

LOG_MODULE_REGISTER(ui_controller_logger);

namespace {

// The object of the widget with this ID among a screen's widgets and, owned, theirs; null if there is none.
// Recursive to the depth of the composition, which the validator holds to 8 levels.
// NOLINTNEXTLINE(misc-no-recursion)
lv_obj_t* FindWidgetObject(std::span<const std::unique_ptr<eerie_leap::views::widgets::IWidget>> widgets, uint32_t id) {
    for(const auto& widget : widgets) {
        if(widget->GetId() == id)
            return widget->GetContainer()->GetObject();

        if(auto* owned = FindWidgetObject(widget->GetChildren(), id))
            return owned;
    }

    return nullptr;
}

} // namespace

namespace {

std::optional<uint32_t> GetPayloadId(const NavigationEventChannel::EventMessage& event, NavigationPayloadType key) {

    const auto* it = event.payload.find(key);
    if(it == event.payload.end())
        return std::nullopt;

    const auto* value = std::get_if<uint32_t>(&it->second);

    return value != nullptr ? std::optional<uint32_t>(*value) : std::nullopt;
}

} // namespace

namespace {

// Where CborConfigurationService keeps a configuration: its directory and the name with an extension.
std::string StoredConfigurationPath(std::string_view name) {
    return "config/" + std::string(name) + ".cbor";
}

} // namespace

UiController::UiController(
    std::shared_ptr<IFsService> fs_service,
    std::shared_ptr<WorkQueueThread> config_work_queue_thread,
    std::shared_ptr<ConfigurationService> configuration_service,
    std::shared_ptr<AssetsManager> assets_manager
) :
    fs_service_(std::move(fs_service)),
    config_work_queue_thread_(std::move(config_work_queue_thread)),
    configuration_service_(std::move(configuration_service)),
    assets_manager_(std::move(assets_manager)),
    dispatch_guard_(std::make_shared<DispatchGuard>(this)),
    rebuild_guard_(std::make_shared<DispatchGuard>(this)) {

    k_mutex_init(&state_replay_mutex_);

    if(assets_manager_ == nullptr)
        assets_manager_ = std::make_shared<AssetsManager>();
}

UiController::~UiController() {
    // Waits for a rebuild in progress, and turns every later one into nothing.
    rebuild_guard_->Detach();
    dispatch_guard_->Detach();

    // A preview in progress is waited for here; the stored screens it would put back are gone with the view.
#ifdef CONFIG_EERIE_LEAP_UI_PREVIEW
    preview_service_.reset();
#endif
    highlight_.reset();

    navigation_subscription_.reset();

    // The dispatcher holds a raw pointer to a work queue destroyed before main_view_.
    if(main_view_ != nullptr)
        main_view_->SetRenderDispatcher(nullptr);

    // The registry outlives the controller. Widgets that are still drawing find nothing and show nothing.
    if(images_registered_)
        assets_manager_->Unregister(AssetKind::IMAGES);

    if(animations_registered_)
        assets_manager_->Unregister(AssetKind::ANIMATIONS);

    if(fonts_registered_)
        assets_manager_->Unregister(AssetKind::FONTS);
}

int UiController::Initialize() {
    ThemeManager::GetInstance().SetTheme(std::make_shared<DarkBWTheme>());

    ui_renderer_service_ = std::make_shared<UiRendererService>();
    if(ui_renderer_service_->Initialize() != 0) {
        LOG_ERR("Failed to initialize the UI renderer service.");
        return -1;
    }

    ui_renderer_service_->Start();

    // Asked before the manager exists: it stores a default configuration as soon as it finds none.
    [[maybe_unused]] const bool has_stored_configuration = fs_service_->Exists(
        StoredConfigurationPath(UI_CONFIGURATION_NAME)
    );

    auto cbor_ui_config_service = std::make_unique<config_services::CborConfigurationService<CborUiConfig>>(
        UI_CONFIGURATION_NAME, fs_service_, config_work_queue_thread_
    );
    ui_configuration_manager_ = std::make_shared<UiConfigurationManager>(
        std::move(cbor_ui_config_service),
        [](const auto& owner, auto children) {
            WidgetFactory::GetInstance().ValidateChildren(owner, children);
        },
        GetEventBindingError
    );

    if(configuration_service_ != nullptr)
        configuration_service_->RegisterCborConfigurationManager(
            ConfigurationService::Type::Ui, ui_configuration_manager_
        );

    RegisterAssets();

    ScopedLvglLock lvgl_guard;

    main_view_ = std::make_unique<MainView>();
    navigation_service_ = std::make_shared<NavigationService>();

    try {
        overlay_host_ = std::make_unique<OverlayHost>(navigation_service_);
    } catch(const std::exception& e) {
        // Only a missing display gets here; the rest of the UI is still usable.
        LOG_ERR("Failed to create the overlay host. %s", e.what());
    }

    ui_render_work_queue_thread_ = std::make_unique<WorkQueueThread>(
        "ui_render_work_q", UI_RENDER_WORK_QUEUE_STACK_SIZE, UI_RENDER_WORK_QUEUE_PRIORITY
    );

    if(ui_render_work_queue_thread_->Initialize()) {
        // Keeps the event bus worker - and the LVGL lock it holds while
        // dispatching - out of the multi-frame first render of a screen group.
        main_view_->SetRenderDispatcher(
            [work_queue = ui_render_work_queue_thread_.get()](const std::function<void()>& work) {
                try {
                    work_queue->Run(work);
                } catch(const std::exception& e) {
                    LOG_ERR("Failed to queue a screen group render. %s", e.what());
                }
            }
        );
    } else {
        LOG_ERR("Failed to start the UI render work queue, rendering inline.");
        ui_render_work_queue_thread_.reset();
    }

    widget_context_ = WidgetContext{
        .assets_manager = assets_manager_, .font_cache = font_cache_, .navigation_service = navigation_service_
    };

    if(Configure(ui_configuration_manager_->Get()) != 0)
        LOG_ERR("Failed to configure the UI from the stored configuration.");

    SubscribeToNavigation();

    ui_input_service_ = std::make_shared<UiInputService>(navigation_service_);
    if(ui_input_service_->Initialize(main_view_->GetContainer()->GetObject()) != 0)
        LOG_ERR("Failed to initialize the UI input service.");

    ConfigureGestures();

#ifdef CONFIG_EERIE_LEAP_UI_PREVIEW
    if(ui_render_work_queue_thread_ != nullptr && config_work_queue_thread_ != nullptr) {
        auto preview_service = std::make_shared<UiPreviewService>(
            *this, *ui_render_work_queue_thread_, *config_work_queue_thread_
        );
        if(preview_service->Initialize())
            preview_service_ = std::move(preview_service);
        else
            LOG_ERR("Failed to initialize the UI preview.");
    } else {
        LOG_WRN("No UI render or configuration work queue, so no UI preview.");
    }
#endif

    // Last, once there is a view to rebuild: a configuration stored from now on replaces the screens.
    ui_configuration_manager_->RegisterConfigurationUpdatedHandler([this, guard = dispatch_guard_] {
        guard->Dispatch([this] {
            OnConfigurationStored();
        });
    });

    return 0;
}

void UiController::RegisterAssets() {
    images_store_ = std::make_shared<AssetStore>(fs_service_, UI_IMAGES_DIR);
    animations_store_ = std::make_shared<AssetStore>(fs_service_, UI_ANIMATIONS_DIR);
    fonts_store_ = std::make_shared<AssetStore>(fs_service_, UI_FONTS_DIR);

    // Widgets keep RAM copies of what they load, so a client may overwrite or delete a file a widget
    // shows: nothing is ever in use, and the change appears at the widget's next render.
    constexpr auto ACCESS = static_cast<uint8_t>(
        ToMask(AssetAccess::READ) | ToMask(AssetAccess::WRITE) | ToMask(AssetAccess::DELETE)
    );

    images_registered_ = assets_manager_->Register(
        AssetProvider{ .kind = AssetKind::IMAGES,
            .name = "images",
            .store = images_store_,
            .extensions = IMAGE_EXTENSIONS,
            .access = ACCESS,
            .max_size = CONFIG_EERIE_LEAP_UI_IMAGE_MAX_SIZE }
    );
    if(!images_registered_)
        LOG_ERR("Failed to register the image assets, the registry has them already.");

    animations_registered_ = assets_manager_->Register(
        AssetProvider{ .kind = AssetKind::ANIMATIONS,
            .name = "animations",
            .store = animations_store_,
            .extensions = ANIMATION_EXTENSIONS,
            .access = ACCESS,
            .max_size = CONFIG_EERIE_LEAP_UI_ANIMATION_MAX_SIZE }
    );
    if(!animations_registered_)
        LOG_ERR("Failed to register the animation assets, the registry has them already.");

    // A font keeps its own RAM copy, like the others.
    fonts_registered_ = assets_manager_->Register(
        AssetProvider{ .kind = AssetKind::FONTS,
            .name = "fonts",
            .store = fonts_store_,
            .extensions = FONT_EXTENSIONS,
            .access = ACCESS,
            .max_size = CONFIG_EERIE_LEAP_UI_FONT_MAX_SIZE }
    );
    if(!fonts_registered_)
        LOG_ERR("Failed to register the font assets, the registry has them already.");

    font_cache_ = std::make_shared<FontCache>(assets_manager_);
}

void UiController::ConfigureGestures() {
    if(ui_input_service_ == nullptr)
        return;

    ui_input_service_->SetGestureMapping(LV_DIR_BOTTOM, NavigationIntent::CloseOverlay);

    // A rebuilt configuration may have no overlay left for the swipe to open.
    if(default_overlay_screen_id_.has_value())
        ui_input_service_->SetGestureMapping(LV_DIR_TOP, NavigationIntent::ShowOverlay, *default_overlay_screen_id_);
    else
        ui_input_service_->SetGestureMapping(LV_DIR_TOP, NavigationIntent::None);
}

int UiController::Start() {
    int res = 0;

    {
        ScopedLvglLock lvgl_guard;
        res = main_view_->Render();

        // Nothing to draw yet, but a rendered host is what gets told about theme changes.
        if(overlay_host_ != nullptr && overlay_host_->Render() != 0)
            LOG_ERR("Failed to render the overlay host.");
    }

    return res;
}

int UiController::Configure(
    std::shared_ptr<UiConfiguration> config,
    bool render_inline,
    std::optional<uint32_t> preferred_group
) {
    configuration_ = std::move(config);

    // Screen and widget construction creates LVGL objects while the renderer
    // thread is already running.
    ScopedLvglLock lvgl_guard;

    default_overlay_screen_id_.reset();

    for(const auto& screen_config : configuration_->screen_configurations) {
        // An overlay is built on demand by OverlayHost; it belongs to no group.
        if(screen_config->is_overlay) {
            if(!default_overlay_screen_id_.has_value() || screen_config->id < *default_overlay_screen_id_)
                default_overlay_screen_id_ = screen_config->id;

            continue;
        }

        auto screen = CreateScreen(screen_config, main_view_->GetGroupContainer(screen_config->screen_group_id));

        if(screen != nullptr)
            main_view_->AddScreen(std::move(screen));
    }

    main_view_->PruneEmptyGroups();

    auto screen_group_ids = main_view_->GetGroupIds();
    if(screen_group_ids.empty()) {
        LOG_ERR("No usable screen groups were configured.");
        return -ENOENT;
    }

    // Before the start group renders, which can throw: navigation then still knows the groups there are.
    navigation_service_->SetGroupIds(screen_group_ids);

    const bool keeps_preferred_group = preferred_group.has_value()
        && std::ranges::find(screen_group_ids, *preferred_group) != screen_group_ids.end();
    int res = main_view_->SetActiveGroup(
        keeps_preferred_group ? *preferred_group : configuration_->active_screen_group_id, render_inline
    );
    if(res != 0) {
        LOG_WRN("Falling back to screen group %u.", screen_group_ids.front());
        res = main_view_->SetActiveGroup(screen_group_ids.front(), render_inline);
    }

    if(auto active_screen_group_id = main_view_->GetActiveGroupId())
        navigation_service_->SetActiveGroupId(*active_screen_group_id);

    RequestSettingsState();

    return res;
}

// Every screen group is built here, not on first render, so one broadcast reaches every binding
// that was just resolved. Settings owners only publish on change, and none of them has yet.
void UiController::RequestSettingsState() {
    static constexpr auto CALLER = GetCallerName();

    SettingsEventsChannel::GetInstance().PublishAsync(
        { .source_id = CALLER.hash, .type = SettingsEventType::StateRequested }
    );
}

int UiController::Reconfigure(std::shared_ptr<UiConfiguration> configuration, std::optional<uint32_t> preferred_group) {
    int res = 0;

    {
        // One hold from the first teardown to the last frame of the new start group: the renderer thread
        // cannot flush a half-built tree, and the display keeps its last frame meanwhile. The lock order is
        // the usual one: this, then a widget's dispatch guard in its destructor.
        ScopedLvglLock lvgl_guard;

        if(overlay_host_ != nullptr)
            overlay_host_->DismissAll();

        // The overlays are gone with the groups they covered, and so is the way back to a group.
        navigation_service_->Reset();
        main_view_->Clear();

        res = Configure(std::move(configuration), true, preferred_group);
        ConfigureGestures();

        // A unit that showed nothing when it started (no screens stored, no demo) never rendered its view, and
        // a group is then only recorded as the active one. The first configuration it is given renders it.
        if(res == 0 && !main_view_->IsReady())
            res = main_view_->Render();
    }

    if(res != 0)
        LOG_ERR("Failed to show the stored UI configuration. Error: %d.", res);

    ReplayState();

    return res;
}

void UiController::OnConfigurationStored() {
#ifdef CONFIG_EERIE_LEAP_UI_PREVIEW
    // The store's own rebuild shows what it stored, so a preview has nothing to put back.
    if(preview_service_ != nullptr)
        preview_service_->OnConfigurationStored();
#endif

    // A rebuild already queued reads the latest configuration when it runs.
    if(is_reconfigure_queued_.exchange(true))
        return;

    const auto rebuild = [this, guard = rebuild_guard_] {
        guard->Dispatch([this] {
            is_reconfigure_queued_ = false;

            try {
                Reconfigure(ui_configuration_manager_->Get());
            } catch(const std::exception& e) {
                LOG_ERR("Failed to rebuild the UI from the stored configuration. %s", e.what());
            } catch(...) {
                LOG_ERR("Failed to rebuild the UI from the stored configuration.");
            }
        });
    };

    if(ui_render_work_queue_thread_ == nullptr) {
        // No queue to rebuild on; the storing thread has to.
        rebuild();

        return;
    }

    try {
        ui_render_work_queue_thread_->Run(rebuild);
    } catch(const std::exception& e) {
        is_reconfigure_queued_ = false;
        LOG_ERR("Failed to queue a rebuild of the UI. %s", e.what());
    }
}

void UiController::SetStateReplay(std::function<void()> replay) {
    std::shared_ptr<const std::function<void()>> slot;
    if(replay)
        slot = std::make_shared<const std::function<void()>>(std::move(replay));

    const ScopedMutex guard(state_replay_mutex_);
    state_replay_.swap(slot);
}

void UiController::ReplayState() {
    std::shared_ptr<const std::function<void()>> replay;
    {
        const ScopedMutex guard(state_replay_mutex_);
        replay = state_replay_;
    }

    if(replay == nullptr)
        return;

    try {
        (*replay)();
    } catch(const std::exception& e) {
        LOG_ERR("Failed to replay the state to the rebuilt UI. %s", e.what());
    } catch(...) {
        LOG_ERR("Failed to replay the state to the rebuilt UI.");
    }
}

void UiController::SubscribeToNavigation() {
    navigation_subscription_ = CreateScopedSubscription(
        NavigationEventChannel::GetInstance(), NavigationEventType::Changed,
        [this](const NavigationEventChannel::EventMessage& event) {
            OnNavigationChanged(event);
        }
    );

    if(navigation_subscription_ == nullptr)
        LOG_ERR("Failed to subscribe to navigation events.");
}

void UiController::OnNavigationChanged(const NavigationEventChannel::EventMessage& event) {
    // The bus no longer locks LVGL around dispatch, and SetActiveGroup renders.
    ScopedLvglLock lvgl_guard;

    try {
        auto action = GetPayloadId(event, NavigationPayloadType::Action);
        if(!action.has_value())
            return;

        switch(static_cast<NavigationAction>(*action)) {
            // An event is delivered some time after it was published, possibly after the UI was rebuilt and the
            // navigation started over (NavigationService::Reset()). The service holds what was asked for last,
            // so an event for anything else is stale: another request or a rebuild came after it.
            case NavigationAction::ShowGroup:
                if(auto screen_group_id = GetPayloadId(event, NavigationPayloadType::TargetGroupId);
                    screen_group_id.has_value() && navigation_service_->GetActiveGroupId() == screen_group_id)
                    ShowGroup(*screen_group_id);

                break;

            case NavigationAction::ShowOverlay:
                if(auto screen_id = GetPayloadId(event, NavigationPayloadType::TargetScreenId);
                    screen_id.has_value() && navigation_service_->GetActiveOverlayId() == screen_id)
                    ShowOverlay(*screen_id);

                break;

            case NavigationAction::CloseOverlay:
                CloseOverlay();
                break;

            default:
                break;
        }
    } catch(const std::exception& e) {
        LOG_ERR("Failed to apply navigation change. %s", e.what());
    } catch(...) {
        LOG_ERR("Failed to apply navigation change.");
    }
}

void UiController::ShowGroup(uint32_t screen_group_id) {
    if(main_view_->SetActiveGroup(screen_group_id) != 0)
        LOG_ERR("Failed to show screen group %u.", screen_group_id);

    // The view is the source of truth; reconcile the service's optimistic state.
    if(auto active_screen_group_id = main_view_->GetActiveGroupId())
        navigation_service_->SetActiveGroupId(*active_screen_group_id);
}

void UiController::ShowOverlay(uint32_t screen_id) {
    // Every failure below has to tell navigation, whose overlay state blocks group
    // switching until something closes it. That state clears as the call is made;
    // only the matching Pop() comes back around through the bus.
    if(overlay_host_ == nullptr) {
        LOG_ERR("No overlay host to show screen %u on.", screen_id);
        navigation_service_->CloseOverlay();

        return;
    }

    const auto configuration = FindOverlayConfiguration(screen_id);
    if(configuration == nullptr) {
        LOG_ERR("Screen %u is not a configured overlay.", screen_id);
        navigation_service_->CloseOverlay();

        return;
    }

    // Built fresh on every show: Pop() destroys the group and everything under it.
    auto screen_group = overlay_host_->CreateGroup(configuration->screen_group_id);
    auto screen = CreateScreen(configuration, screen_group->GetContainer());
    if(screen == nullptr) {
        LOG_ERR("Failed to build overlay screen %u.", screen_id);
        navigation_service_->CloseOverlay();

        return;
    }

    screen_group->AddScreen(std::move(screen));

    if(overlay_host_->Push(std::move(screen_group)) != 0) {
        LOG_ERR("Failed to show overlay screen %u.", screen_id);
        navigation_service_->CloseOverlay();

        return;
    }

    // This screen did not exist for the broadcast in Configure(), and settings owners
    // only publish on change, so its bindings would sit on their defaults.
    RequestSettingsState();
}

void UiController::CloseOverlay() {
    // -ENOENT is expected here: a failed show reconciles by asking navigation to close.
    if(overlay_host_ != nullptr)
        overlay_host_->Pop();
}

std::shared_ptr<NavigationService> UiController::GetNavigationService() const {
    return navigation_service_;
}

bool UiController::IsNavigationEnabled() const {
    return ui_input_service_ == nullptr || ui_input_service_->IsNavigationEnabled();
}

std::shared_ptr<UiPreviewService> UiController::GetPreviewService() const {
    return preview_service_;
}

std::shared_ptr<UiConfiguration>
UiController::ParseConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) {
    return ui_configuration_manager_->ParseCborConfiguration(cbor_data, reason);
}

bool UiController::ValidateConfiguration(const UiConfiguration& configuration, std::span<char> reason) {
    try {
        ui_configuration_manager_->ValidateConfiguration(configuration);

        return true;
    } catch(const std::exception& e) {
        if(!reason.empty()) {
            const size_t length = std::min(std::strlen(e.what()), reason.size() - 1);
            std::copy_n(e.what(), length, reason.begin());
            reason[length] = '\0';
        }

        return false;
    }
}

// The view cannot add or drop a screen group, or an overlay, nor move a screen between groups, in place: those
// are left to a whole show, which keeps the place.
size_t UiController::GetEncodedSize(const UiConfiguration& configuration) {
    return ui_configuration_manager_->GetEncodedSize(configuration);
}

int UiController::PatchScreen(
    const ScreenPatch& patch,
    std::vector<uint32_t>& overlays_to_reshow,
    bool& has_bound_update
) {
    const auto& old_screen = patch.old_screen;
    const auto& new_screen = patch.new_screen;
    const bool is_overlay = (old_screen != nullptr && old_screen->is_overlay)
        || (new_screen != nullptr && new_screen->is_overlay);

    if(is_overlay) {
        const bool is_kept = patch.action == ScreenAction::Widgets || patch.action == ScreenAction::Rebuild;
        const bool is_same_kind = old_screen != nullptr
            && new_screen != nullptr
            && old_screen->is_overlay == new_screen->is_overlay;
        if(!is_kept || !is_same_kind)
            return PATCH_NEEDS_REBUILD;

        overlays_to_reshow.push_back(patch.screen_id);

        return 0;
    }

    if(patch.action == ScreenAction::Widgets) {
        const auto screen = main_view_->GetScreen(patch.screen_id);
        if(screen == nullptr)
            return PATCH_NEEDS_REBUILD;

#ifdef CONFIG_ZTEST
        if(patch_faults_after_ == 0)
            throw std::runtime_error("A patch of a screen failed (injected).");

        if(patch_faults_after_ != SIZE_MAX)
            --patch_faults_after_;
#endif

        screen->ApplyWidgetPatches(new_screen, patch.widgets);

        // A property an event drives keeps its value, and gets the event again only if it is asked to publish.
        for(const auto& widget_patch : patch.widgets) {
            if(widget_patch.action == WidgetAction::Update
                && widget_patch.widget != nullptr
                && !widget_patch.widget->bindings.empty())
                has_bound_update = true;
        }

        return 0;
    }

    if(patch.action == ScreenAction::Remove)
        return main_view_->RemoveScreen(patch.screen_id) ? 0 : PATCH_NEEDS_REBUILD;

    // Rebuild or Add: the screen is built again, in its group, which has to be there.
    const auto group_ids = main_view_->GetGroupIds();
    const bool is_moved = old_screen != nullptr && old_screen->screen_group_id != new_screen->screen_group_id;
    if(is_moved || std::ranges::find(group_ids, new_screen->screen_group_id) == group_ids.end())
        return PATCH_NEEDS_REBUILD;

    const auto screen = CreateScreen(new_screen, main_view_->GetGroupContainer(new_screen->screen_group_id));
    if(screen == nullptr)
        return -EIO;

    return main_view_->PutScreen(screen) ? 0 : PATCH_NEEDS_REBUILD;
}

// The screens of a group are drawn by z-index, and equal ones in the order the configuration lists them, which is
// what a whole stage of this configuration would give: a screen that was put again is where the list has it. Every
// group is put in order, not only those a patch names: a screen removed and put again in one request changes the
// order of the list and nothing else.
void UiController::OrderPatchedGroups(const UiConfiguration& patched) {
    std::vector<uint32_t> group_ids;
    for(const auto& screen : patched.screen_configurations) {
        if(!screen->is_overlay && std::ranges::find(group_ids, screen->screen_group_id) == group_ids.end())
            group_ids.push_back(screen->screen_group_id);
    }

    for(const auto group_id : group_ids) {
        std::vector<std::shared_ptr<ScreenConfiguration>> members;
        for(const auto& screen : patched.screen_configurations) {
            if(!screen->is_overlay && screen->screen_group_id == group_id)
                members.push_back(screen);
        }

        std::ranges::stable_sort(members, {}, [](const auto& screen) {
            return screen->z_index;
        });
        std::vector<uint32_t> ids;
        ids.reserve(members.size());
        for(const auto& screen : members)
            ids.push_back(screen->id);

        main_view_->OrderScreens(group_id, ids);
    }
}

int UiController::ApplyPatch(std::shared_ptr<UiConfiguration> patched, PatchPlan& plan) {
    bool is_replay_needed = false;
    int res = 0;
    {
        ScopedLvglLock lvgl_guard;
        res = ApplyPatchLocked(std::move(patched), plan, is_replay_needed);
    }

    // As after a rebuild: what the new widgets bind to publishes only on change, so they are told once the screens
    // stand, outside the LVGL lock like Reconfigure() does it.
    if(res == 0 && is_replay_needed) {
        RequestSettingsState();
        ReplayState();
    }

    return res;
}

int UiController::ApplyPatchLocked(std::shared_ptr<UiConfiguration> patched, PatchPlan& plan, bool& is_replay_needed) {

    if(configuration_ == nullptr || main_view_ == nullptr || patched == nullptr)
        return -EINVAL;

    // Every overlay a change was made on, whether it is up now or not.
    std::vector<uint32_t> overlays_to_reshow;
    int outcome = 0;
    bool has_bound_update = false;
    try {
        for(const auto& patch : plan.screens) {
            outcome = PatchScreen(patch, overlays_to_reshow, has_bound_update);
            if(outcome != 0)
                break;
        }
    } catch(const std::exception& e) {
        LOG_ERR("Failed to patch the screens. %s", e.what());

        return -EIO;
    } catch(...) {
        LOG_ERR("Failed to patch the screens.");

        return -EIO;
    }

    if(outcome < 0)
        return outcome;

    if(outcome == PATCH_NEEDS_REBUILD) {
        UiPreviewApplied applied;
        const int res = ShowConfiguration(std::move(patched), applied);
        std::ranges::fill(plan.effects, PatchEffect::GroupRebuilt);

        return res == -ENOENT ? 0 : res;
    }

    OrderPatchedGroups(*patched);

    // Widgets built again have not been told what they bind to, and neither have bound ones put in place.
    is_replay_needed = has_bound_update || std::ranges::any_of(plan.effects, [](PatchEffect effect) {
        return effect == PatchEffect::WidgetRebuilt || effect == PatchEffect::GroupRebuilt;
    });

    // Overlays are built from the configuration when they are shown.
    const auto active_overlay = navigation_service_->GetActiveOverlayId();
    configuration_ = patched;

    if(active_overlay.has_value()
        && std::ranges::find(overlays_to_reshow, *active_overlay) != overlays_to_reshow.end()) {
        navigation_service_->CloseOverlay();
        navigation_service_->ShowOverlay(*active_overlay);
    }

    // A change on an overlay shows only where the overlay is up; an operation that changed nothing on it is 0.
    for(size_t i = 0; i < plan.effects.size() && i < plan.op_screen_ids.size(); ++i) {
        const auto screen_id = plan.op_screen_ids[i];
        if(std::ranges::find(overlays_to_reshow, screen_id) != overlays_to_reshow.end())
            plan.effects[i] = active_overlay == screen_id ? PatchEffect::GroupRebuilt : PatchEffect::NotShown;
    }

    return 0;
}

int UiController::ShowConfiguration(std::shared_ptr<UiConfiguration> configuration, UiPreviewApplied& applied) {
    // Where the gauge stands is where it stays, as far as the new screens go on: the editor changes a screen, not
    // the one the user is looking at. Navigation knows at once what was asked for last.
    const auto group = navigation_service_->GetActiveGroupId();
    const auto overlay = navigation_service_->GetActiveOverlayId();

    bool has_overlay = false;
    if(overlay.has_value()) {
        has_overlay = std::ranges::any_of(configuration->screen_configurations, [&overlay](const auto& screen) {
            return screen->is_overlay && screen->id == *overlay;
        });
    }

    const int res = Reconfigure(std::move(configuration), group);

    applied = {};
    if(res != 0)
        return res;

    applied.group = navigation_service_->GetActiveGroupId();

    // The rebuild closed the overlay; it comes back through navigation, like any other.
    if(has_overlay && navigation_service_->ShowOverlay(*overlay) == 0)
        applied.overlay = overlay;

    return res;
}

int UiController::ShowStoredConfiguration() {
    return Reconfigure(ui_configuration_manager_->Get());
}

void UiController::SetNavigationPinned(bool is_pinned) {
    if(ui_input_service_ != nullptr)
        ui_input_service_->SetNavigationEnabled(!is_pinned);
}

uint32_t UiController::GetStoredCrc() const {
    return ui_configuration_manager_->GetCborConfigurationInfo().crc;
}

std::optional<uint32_t> UiController::GetActiveGroupId() const {
    return navigation_service_ != nullptr ? navigation_service_->GetActiveGroupId() : std::nullopt;
}

UiPreviewError UiController::ShowTarget(const UiPreviewTarget& target, UiPreviewShown& shown) {
    ScopedLvglLock lvgl_guard;

    if(configuration_ == nullptr)
        return UiPreviewError::NOT_FOUND;

    if(target.is_overlay) {
        if(FindOverlayConfiguration(target.id) == nullptr)
            return UiPreviewError::NOT_FOUND;

        // One overlay at a time: the one up is closed first. The events are delivered in order.
        if(navigation_service_->IsOverlayActive())
            navigation_service_->CloseOverlay();

        if(navigation_service_->ShowOverlay(target.id) != 0)
            return UiPreviewError::UNKNOWN;

        shown.group = navigation_service_->GetActiveGroupId().value_or(configuration_->active_screen_group_id);
        shown.overlay = target.id;

        return UiPreviewError::OK;
    }

    const auto group_ids = main_view_->GetGroupIds();
    if(std::ranges::find(group_ids, target.id) == group_ids.end())
        return UiPreviewError::NOT_FOUND;

    if(navigation_service_->IsOverlayActive())
        navigation_service_->CloseOverlay();

    if(navigation_service_->GoToGroup(target.id) != 0)
        return UiPreviewError::UNKNOWN;

    shown.group = target.id;
    shown.overlay.reset();

    return UiPreviewError::OK;
}

lv_obj_t* UiController::ResolveHighlightTarget(std::optional<uint32_t> screen, std::optional<uint32_t> widget) const {
    std::vector<std::shared_ptr<IScreen>> candidates;

    const auto* overlay_group = overlay_host_ != nullptr ? overlay_host_->GetTopGroup() : nullptr;
    if(screen.has_value()) {
        if(auto found = main_view_->GetScreen(*screen))
            candidates.push_back(std::move(found));
        else if(overlay_group != nullptr)
            if(auto in_overlay = overlay_group->GetScreen(*screen))
                candidates.push_back(std::move(in_overlay));
    } else {
        // A widget alone: the overlay that is up, then the group that is shown.
        if(overlay_group != nullptr)
            candidates = overlay_group->GetScreens();

        if(const auto active = main_view_->GetActiveGroupId()) {
            const auto group_screens = main_view_->GetGroupScreens(*active);
            candidates.insert(candidates.end(), group_screens.begin(), group_screens.end());
        }
    }

    for(const auto& candidate : candidates) {
        if(!widget.has_value())
            return candidate->GetContainer()->GetObject();

        if(auto* found = FindWidgetObject(*candidate->GetWidgets(), *widget))
            return found;
    }

    return nullptr;
}

std::optional<uint32_t>
UiController::FindHighlightScreen(std::optional<uint32_t> screen, std::optional<uint32_t> widget) const {
    if(configuration_ == nullptr)
        return std::nullopt;

    const auto has_widget = [&widget](const ScreenConfiguration& configuration) {
        return !widget.has_value()
            || std::ranges::any_of(configuration.widget_configurations, [&widget](const auto& definition) {
                   return definition->id == *widget;
               });
    };

    if(screen.has_value()) {
        for(const auto& candidate : configuration_->screen_configurations) {
            if(candidate->id == *screen)
                return has_widget(*candidate) ? screen : std::nullopt;
        }

        return std::nullopt;
    }

    // A widget alone: the overlay navigation holds up, then the group it stands on. What it has been asked for is
    // what counts, whether or not the screens have caught up with it.
    if(const auto overlay_id = navigation_service_->GetActiveOverlayId()) {
        if(const auto overlay = FindOverlayConfiguration(*overlay_id); overlay != nullptr && has_widget(*overlay))
            return overlay->id;
    }

    if(const auto group_id = navigation_service_->GetActiveGroupId()) {
        for(const auto& candidate : configuration_->screen_configurations) {
            if(!candidate->is_overlay && candidate->screen_group_id == *group_id && has_widget(*candidate))
                return candidate->id;
        }
    }

    return std::nullopt;
}

UiPreviewError UiController::Highlight(std::optional<uint32_t> screen, std::optional<uint32_t> widget) {
    ScopedLvglLock lvgl_guard;

    if(!screen.has_value() && !widget.has_value()) {
        if(highlight_ != nullptr)
            highlight_->Clear();

        return UiPreviewError::OK;
    }

    // Judged against the configuration, not the objects built from it: an overlay is built when navigation gets
    // to it, and the outline waits for it.
    const auto screen_id = FindHighlightScreen(screen, widget);
    if(!screen_id.has_value())
        return UiPreviewError::NOT_FOUND;

    if(highlight_ == nullptr) {
        // Resolved again at every tick: the screens may have been rebuilt, or the widget moved. A widget on a group that
        // is not shown has nothing to outline.
        highlight_ = std::make_unique<UiHighlight>([this](std::optional<uint32_t> s, std::optional<uint32_t> w) {
            auto* object = ResolveHighlightTarget(s, w);

            return object != nullptr && Frame::IsVisibleInHierarchy(object) ? object : nullptr;
        });
    }

    highlight_->Set(screen_id, widget);

    return UiPreviewError::OK;
}

bool UiController::IsShowingScreens() const {
    ScopedLvglLock lvgl_guard;

    return main_view_ != nullptr && main_view_->IsReady() && main_view_->GetActiveGroupId().has_value();
}

std::shared_ptr<UiConfigurationManager> UiController::GetConfigurationManager() const {
    return ui_configuration_manager_;
}

std::shared_ptr<IScreen>
UiController::CreateScreen(const std::shared_ptr<ScreenConfiguration>& configuration, std::shared_ptr<Frame> parent) {

    try {
        return ScreenFactory::GetInstance().CreateScreen(configuration, std::move(parent), widget_context_);
    } catch(const std::exception& e) {
        LOG_ERR("Failed to create screen with ID: %d. %s", configuration->id, e.what());
        return nullptr;
    }
}

std::shared_ptr<ScreenConfiguration> UiController::FindOverlayConfiguration(uint32_t screen_id) const {
    if(configuration_ == nullptr)
        return nullptr;

    for(const auto& screen_config : configuration_->screen_configurations)
        if(screen_config->is_overlay && screen_config->id == screen_id)
            return screen_config;

    return nullptr;
}

} // namespace eerie_leap::controllers
