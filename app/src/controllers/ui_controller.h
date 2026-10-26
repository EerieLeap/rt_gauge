#pragma once

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <span>
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

#include "domain/ui_preview_domain/i_ui_preview_host.h"
#include "domain/ui_preview_domain/ui_preview_service.h"

#include "views/main_view.h"
#include "views/overlay_host.h"
#include "views/screens/i_screen.h"
#include "views/ui_highlight.h"
#include "views/utilities/font_cache.h"
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

using eerie_leap::domain::ui_domain::preview::PatchPlan;
using eerie_leap::domain::ui_domain::preview::ScreenPatch;
using eerie_leap::domain::ui_domain::preview::UiPreviewError;
using eerie_leap::domain::ui_domain::preview::WidgetAction;
using eerie_leap::domain::ui_preview_domain::IUiPreviewHost;
using eerie_leap::domain::ui_preview_domain::UiPreviewApplied;
using eerie_leap::domain::ui_preview_domain::UiPreviewService;
using eerie_leap::domain::ui_preview_domain::UiPreviewShown;
using eerie_leap::domain::ui_preview_domain::UiPreviewTarget;
using eerie_leap::views::MainView;
using eerie_leap::views::OverlayHost;
using eerie_leap::views::UiHighlight;
using eerie_leap::views::screens::IScreen;
using eerie_leap::views::utilities::FontCache;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::widgets::WidgetContext;

class UiController final : public IUiPreviewHost {
private:
    static constexpr const char* UI_CONFIGURATION_NAME = "ui_config";
    static constexpr const char* UI_ASSETS_DIR = "ui_assets";
    static constexpr const char* UI_IMAGES_DIR = "ui_assets/images";
    static constexpr const char* UI_ANIMATIONS_DIR = "ui_assets/animations";
    static constexpr const char* UI_FONTS_DIR = "ui_assets/fonts";

    // What a client may upload as each kind. A provider keeps only a span of its extensions.
    static constexpr std::array<std::string_view, 2> IMAGE_EXTENSIONS{ ".bin", ".svg" };
    static constexpr std::array<std::string_view, 1> ANIMATION_EXTENSIONS{ ".json" };
    static constexpr std::array<std::string_view, 1> FONT_EXTENSIONS{ ".ttf" };

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
    std::shared_ptr<AssetStore> fonts_store_;
    // Whether the registry took the kind from this controller, so only that is taken back.
    bool images_registered_ = false;
    bool animations_registered_ = false;
    bool fonts_registered_ = false;
    // Finds the fonts kind through the registry, so it works whichever controller registered it.
    std::shared_ptr<FontCache> font_cache_;
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
    // @param preferred_group The group to start on when the configuration has it, instead of its own start group.
    int Configure(
        std::shared_ptr<UiConfiguration> config,
        bool render_inline = false,
        std::optional<uint32_t> preferred_group = std::nullopt
    );

    // Replaces the screens with those of a stored configuration, in place and in one hold of the LVGL lock:
    // overlays are dismissed, the main view is cleared, the configuration is built with its start group
    // rendered at once (the last frame stays up meanwhile, with no blank one between), the gestures are armed
    // again, and then the state the new widgets missed is asked for and replayed. Navigation starts over at
    // the configuration's start group. Runs on the render work queue.
    int
    Reconfigure(std::shared_ptr<UiConfiguration> configuration, std::optional<uint32_t> preferred_group = std::nullopt);

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

    // The outline a preview puts over a widget; made when the first one is asked for.
    std::unique_ptr<UiHighlight> highlight_;
    // The preview of a staged configuration. Declared after the work queues it runs on, so it goes first.
    std::shared_ptr<UiPreviewService> preview_service_;

    // The object a highlight outlines; the LVGL lock is held. Null if the screens have no such widget or screen.
    lv_obj_t* ResolveHighlightTarget(std::optional<uint32_t> screen, std::optional<uint32_t> widget) const;
    // The screen of the configuration shown that a highlight names, built or not yet: the screen itself, or, for a
    // widget alone, the first screen of the overlay up and then of the group navigation stands on that has it.
    std::optional<uint32_t> FindHighlightScreen(std::optional<uint32_t> screen, std::optional<uint32_t> widget) const;

    // Offers ui_assets/images, ui_assets/animations and ui_assets/fonts to clients as the image, animation and font
    // kinds.
    void RegisterAssets();

    std::shared_ptr<IScreen>
    CreateScreen(const std::shared_ptr<ScreenConfiguration>& configuration, std::shared_ptr<Frame> parent);
    std::shared_ptr<ScreenConfiguration> FindOverlayConfiguration(uint32_t screen_id) const;

    // Asks every settings owner to republish, so a binding resolved just now is not left showing
    // its default until the value happens to change.
    static void RequestSettingsState();

    // What PatchScreen() answers beside 0 and an error: the view cannot do this one in place.
    static constexpr int PATCH_NEEDS_REBUILD = 1;
    int PatchScreen(const ScreenPatch& patch, std::vector<uint32_t>& overlays_to_reshow, bool& has_bound_update);
    void OrderPatchedGroups(const UiConfiguration& patched);
    // ApplyPatch() under the LVGL lock; @p is_replay_needed says the state is to be published again after it.
    int ApplyPatchLocked(std::shared_ptr<UiConfiguration> patched, PatchPlan& plan, bool& is_replay_needed);
#ifdef CONFIG_ZTEST
    // Tests only: the patch of this many screens succeeds, and the next one fails.
    size_t patch_faults_after_ = SIZE_MAX;
#endif

    void SubscribeToNavigation();
    void OnNavigationChanged(const NavigationEventChannel::EventMessage& event);
    void ShowGroup(uint32_t screen_group_id);
    void ShowOverlay(uint32_t screen_id);
    void CloseOverlay();
    void ConfigureGestures();

public:
    /**
     * @param assets_manager The registry the image, animation and font kinds are registered in on
     *        Initialize() and removed from on destruction; a registry of its own when null.
     */
    UiController(
        std::shared_ptr<IFsService> fs_service,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread,
        std::shared_ptr<ConfigurationService> configuration_service,
        std::shared_ptr<AssetsManager> assets_manager = nullptr
    );
    // Not under the LVGL lock: it waits for a rebuild in progress, which holds its guard and then takes that lock.
    ~UiController() override;
    // The subscriptions, the render dispatcher and the registered asset kinds refer to it.
    UiController(const UiController&) = delete;
    UiController& operator=(const UiController&) = delete;
    UiController(UiController&&) = delete;
    UiController& operator=(UiController&&) = delete;

    int Initialize();
    int Start();

    std::shared_ptr<NavigationService> GetNavigationService() const;
    std::shared_ptr<UiConfigurationManager> GetConfigurationManager() const;
    // Whether a screen group is on the display: false for a unit that stores no screens.
    bool IsShowingScreens() const;

    // The UI preview (IUiPreviewHost): the screens a staged configuration is shown on.
    std::shared_ptr<UiConfiguration>
    ParseConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) override;
    bool ValidateConfiguration(const UiConfiguration& configuration, std::span<char> reason) override;
    size_t GetEncodedSize(const UiConfiguration& configuration) override;
    int ApplyPatch(std::shared_ptr<UiConfiguration> patched, PatchPlan& plan) override;
    int ShowConfiguration(std::shared_ptr<UiConfiguration> configuration, UiPreviewApplied& applied) override;
    int ShowStoredConfiguration() override;
    void SetNavigationPinned(bool is_pinned) override;
    UiPreviewError ShowTarget(const UiPreviewTarget& target, UiPreviewShown& shown) override;
    UiPreviewError Highlight(std::optional<uint32_t> screen, std::optional<uint32_t> widget) override;
    [[nodiscard]] uint32_t GetStoredCrc() const override;
    [[nodiscard]] std::optional<uint32_t> GetActiveGroupId() const override;

    /** @brief Whether the swipes navigate: not while a preview pins the screen. */
    bool IsNavigationEnabled() const;

#ifdef CONFIG_ZTEST
    /** @brief Tests only: after @p screens screens are patched, the next patch of a screen throws. */
    void FailPatchAfter(size_t screens) {
        patch_faults_after_ = screens;
    }
#endif

    /** @brief The preview of staged configurations; null when the build has none or its work queues did not start. */
    std::shared_ptr<UiPreviewService> GetPreviewService() const;

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
