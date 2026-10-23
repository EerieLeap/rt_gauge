#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <zephyr/kernel.h>

#include "subsys/assets/asset_store.h"
#include "subsys/assets/assets_manager.h"
#include "subsys/event_bus/dispatch_guard.h"
#include "subsys/event_bus/i_scoped_subscription.h"
#include "subsys/fs/services/i_fs_service.h"
#include "subsys/threading/work_queue_thread.h"

#include "domain/configuration_domain/services/configuration_service.h"

#include "domain/ui_domain/configuration/ui_configuration_manager.h"
#include "domain/ui_domain/event_bus/navigation_event_channel.h"
#include "domain/ui_domain/models/screen_configuration.h"
#include "domain/ui_domain/models/ui_configuration.h"
#include "domain/ui_domain/services/navigation_service.h"
#include "domain/ui_domain/services/ui_input_service.h"
#include "domain/ui_domain/services/ui_renderer_service.h"

#include "views/main_view.h"
#include "views/overlay_host.h"
#include "views/screens/i_screen.h"
#include "views/utilitites/frame.h"
#include "views/widgets/widget_context.h"

namespace eerie_leap::controllers {

using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::subsys::assets::AssetStore;
using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::subsys::threading::WorkQueueThread;

using eerie_leap::domain::configuration_domain::services::ConfigurationService;

using eerie_leap::domain::ui_domain::configuration::UiConfigurationManager;
using eerie_leap::domain::ui_domain::event_bus::NavigationEventChannel;
using eerie_leap::domain::ui_domain::models::ScreenConfiguration;
using eerie_leap::domain::ui_domain::models::UiConfiguration;
using eerie_leap::domain::ui_domain::services::NavigationService;
using eerie_leap::domain::ui_domain::services::UiInputService;
using eerie_leap::domain::ui_domain::services::UiRendererService;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::subsys::event_bus::DispatchGuard;

using eerie_leap::views::MainView;
using eerie_leap::views::OverlayHost;
using eerie_leap::views::screens::IScreen;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::widgets::WidgetContext;

class UiController {
private:
    static constexpr const char* UI_CONFIGURATION_NAME = "ui_config";
    // The directory every asset used to share, which SetupTestAssets() still clears, and the one of each kind.
    static constexpr const char* UI_ASSETS_DIR = "ui_assets";
    static constexpr const char* UI_IMAGES_DIR = "ui_assets/images";
    static constexpr const char* UI_ANIMATIONS_DIR = "ui_assets/animations";

    // What a client may upload as each kind. A provider keeps only a span of its extensions.
    static constexpr std::array<std::string_view, 1> IMAGE_EXTENSIONS{ ".bin" };
    static constexpr std::array<std::string_view, 1> ANIMATION_EXTENSIONS{ ".json" };

    // The first render of a screen group builds its whole LVGL tree, so it gets
    // a stack of its own instead of running on the event bus worker.
    static constexpr int UI_RENDER_WORK_QUEUE_STACK_SIZE = CONFIG_EERIE_LEAP_UI_RENDER_WORK_QUEUE_STACK_SIZE;
    static constexpr int UI_RENDER_WORK_QUEUE_PRIORITY = 9;

    std::shared_ptr<IFsService> fs_service_;
    std::shared_ptr<WorkQueueThread> config_work_queue_thread_;
    std::shared_ptr<ConfigurationService> configuration_service_;

    std::shared_ptr<UiConfigurationManager> ui_configuration_manager_;
    // The unit's registry of asset kinds, shared with the other controllers; the kinds this controller
    // serves are the ones it registers there.
    std::shared_ptr<AssetsManager> assets_manager_;
    std::shared_ptr<AssetStore> images_store_;
    std::shared_ptr<AssetStore> animations_store_;
    // Whether the registry took the kind from this controller, so only that is taken back.
    bool images_registered_ = false;
    bool animations_registered_ = false;
    std::shared_ptr<NavigationService> navigation_service_;

    // Declared before ui_input_service_, which detaches from the view's LVGL object on teardown.
    std::unique_ptr<MainView> main_view_;
    std::unique_ptr<OverlayHost> overlay_host_;
    std::shared_ptr<UiInputService> ui_input_service_;

    std::unique_ptr<WorkQueueThread> ui_render_work_queue_thread_;

    // Declared last so its thread stops calling lv_timer_handler() before any
    // of the LVGL objects above are freed.
    std::shared_ptr<UiRendererService> ui_renderer_service_;

    WidgetContext widget_context_;
    std::shared_ptr<UiConfiguration> configuration_;

    // What a swipe-down opens, resolved from the configured overlays.
    std::optional<uint32_t> default_overlay_screen_id_;

    AnySubscription navigation_subscription_;

    // Builds the screens of a configuration into the (empty) main view and shows its start group.
    // @param render_inline Renders the start group here instead of on the render work queue.
    int Configure(std::shared_ptr<UiConfiguration> config, bool render_inline = false);

    // Replaces the screens with those of a stored configuration, in place and in one hold of the LVGL lock:
    // overlays are dismissed, the main view is cleared, the configuration is built with its start group
    // rendered at once (the last frame stays up meanwhile, with no blank one between), the gestures are armed
    // again, and then the state the new widgets missed is asked for and replayed. Navigation starts over at
    // the configuration's start group. Runs on the render work queue.
    int Reconfigure(std::shared_ptr<UiConfiguration> configuration);

    // The manager's updated handler, on the storing thread: queues one Reconfigure() of the latest stored
    // configuration, however many stores come before it runs.
    void OnConfigurationStored();

    // What the new widgets' bindings missed, which no owner publishes again until it changes: the settings
    // (asked for), and what StateReplay brings.
    void ReplayState();

    // Set from any thread; read by ReplayState() on the render work queue.
    mutable k_mutex state_replay_mutex_{};
    std::shared_ptr<const std::function<void()>> state_replay_;

    // The updated handler and the queued rebuild hold one each instead of this, and the destructor detaches
    // both first, so neither reaches a destroyed controller. Apart, because the rebuild holds its guard for
    // all of its run, and the handler, on the thread that stores, must not wait for it.
    std::shared_ptr<DispatchGuard> dispatch_guard_;
    std::shared_ptr<DispatchGuard> rebuild_guard_;
    std::atomic<bool> is_reconfigure_queued_ = false;

    // Offers ui_assets/images and ui_assets/animations to clients as the image and animation kinds.
    void RegisterAssets();

    std::shared_ptr<IScreen>
    CreateScreen(const std::shared_ptr<ScreenConfiguration>& configuration, std::shared_ptr<Frame> parent);
    std::shared_ptr<ScreenConfiguration> FindOverlayConfiguration(uint32_t screen_id) const;

    // Asks every settings owner to republish, so a binding resolved just now is not left showing
    // its default until the value happens to change.
    static void RequestSettingsState();

    void SubscribeToNavigation();
    void OnNavigationChanged(const NavigationEventChannel::EventMessage& event);
    void ShowGroup(uint32_t screen_group_id);
    void ShowOverlay(uint32_t screen_id);
    void CloseOverlay();
    void ConfigureGestures();

#ifdef CONFIG_EERIE_LEAP_UI_DEMO_CONFIGURATION
    // TODO: For test purposes only
    // The demo screens, stored when the unit stores no UI configuration.
    void SetupTestConfiguration();
    // The demo files the demo screens show, where they are missing.
    void SetupTestAssets();
#endif

public:
    /**
     * @param assets_manager The registry the image and animation kinds are registered in on
     *        Initialize() and removed from on destruction; a registry of its own when null.
     */
    UiController(
        std::shared_ptr<IFsService> fs_service,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread,
        std::shared_ptr<ConfigurationService> configuration_service,
        std::shared_ptr<AssetsManager> assets_manager = nullptr
    );
    ~UiController();
    // The subscriptions, the render dispatcher and the registered asset kinds refer to it.
    UiController(const UiController&) = delete;
    UiController& operator=(const UiController&) = delete;
    UiController(UiController&&) = delete;
    UiController& operator=(UiController&&) = delete;

    int Initialize();
    int Start();

    std::shared_ptr<NavigationService> GetNavigationService() const;
    std::shared_ptr<UiConfigurationManager> GetConfigurationManager() const;

    /**
     * @brief Sets what Reconfigure() replays once the new screens stand, after it has asked every settings
     *        owner to publish again: whatever else only publishes on change, such as the sensors' last
     *        values and the logging status.
     *
     * Built after this controller, those owners are handed over here. Callable from any thread; it runs on
     * the render work queue, and must not block.
     */
    void SetStateReplay(std::function<void()> replay);
};

} // namespace eerie_leap::controllers
