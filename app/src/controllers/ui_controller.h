#pragma once

#include <memory>
#include <optional>
#include <vector>

#include "subsys/assets/assets_manager.h"
#include "subsys/event_bus/i_scoped_subscription.h"
#include "subsys/fs/services/i_fs_service.h"
#include "subsys/threading/work_queue_thread.h"

#include "domain/configuration_domain/services/configuration_service.h"
#include "domain/sensor_domain/utilities/sensor_readings_frame.hpp"

#include "domain/ui_domain/configuration/ui_configuration_manager.h"
#include "domain/ui_domain/event_bus/navigation_event_channel.h"
#include "domain/ui_domain/models/ui_configuration.h"
#include "domain/ui_domain/models/screen_configuration.h"
#include "domain/ui_domain/services/ui_renderer_service.h"
#include "domain/ui_domain/services/sensors_rendering_service.h"
#include "domain/ui_domain/services/navigation_service.h"
#include "domain/ui_domain/services/ui_input_service.h"

#include "views/main_view.h"
#include "views/overlay_host.h"
#include "views/screens/i_screen.h"
#include "views/utilitites/frame.h"
#include "views/widgets/widget_context.h"

namespace eerie_leap::controllers {

using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::subsys::fs::services::IFsService;
using eerie_leap::subsys::threading::WorkQueueThread;

using eerie_leap::domain::configuration_domain::services::ConfigurationService;
using eerie_leap::domain::sensor_domain::utilities::SensorReadingsFrame;

using eerie_leap::domain::ui_domain::models::UiConfiguration;
using eerie_leap::domain::ui_domain::models::ScreenConfiguration;
using eerie_leap::domain::ui_domain::configuration::UiConfigurationManager;
using eerie_leap::domain::ui_domain::event_bus::NavigationEventChannel;
using eerie_leap::subsys::event_bus::AnySubscription;
using eerie_leap::domain::ui_domain::services::UiRendererService;
using eerie_leap::domain::ui_domain::services::SensorsRenderingService;
using eerie_leap::domain::ui_domain::services::NavigationService;
using eerie_leap::domain::ui_domain::services::UiInputService;

using eerie_leap::views::MainView;
using eerie_leap::views::OverlayHost;
using eerie_leap::views::screens::IScreen;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::widgets::WidgetContext;

class UiController {
private:
    static constexpr const char* UI_CONFIGURATION_NAME = "ui_config";
    static constexpr const char* UI_ASSETS_DIR = "ui_assets";

    // The first render of a screen group builds its whole LVGL tree, so it gets
    // a stack of its own instead of running on the event bus worker.
    static constexpr int ui_render_work_queue_stack_size_ = CONFIG_EERIE_LEAP_UI_RENDER_WORK_QUEUE_STACK_SIZE;
    static constexpr int ui_render_work_queue_priority_ = 9;

    std::shared_ptr<IFsService> fs_service_;
    std::shared_ptr<WorkQueueThread> config_work_queue_thread_;
    std::shared_ptr<ConfigurationService> configuration_service_;
    std::shared_ptr<SensorReadingsFrame> sensor_readings_frame_;

    std::shared_ptr<UiConfigurationManager> ui_configuration_manager_;
    std::shared_ptr<AssetsManager> ui_assets_manager_;
    std::shared_ptr<SensorsRenderingService> sensors_rendering_service_;
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
    int Configure(std::shared_ptr<UiConfiguration> config);

    std::shared_ptr<IScreen> CreateScreen(std::shared_ptr<ScreenConfiguration> configuration, std::shared_ptr<Frame> parent);
    std::shared_ptr<ScreenConfiguration> FindOverlayConfiguration(uint32_t screen_id) const;

    // Asks every settings owner to republish, so a binding resolved just now is not left showing
    // its default until the value happens to change.
    void RequestSettingsState() const;

    void SubscribeToNavigation();
    void OnNavigationChanged(const NavigationEventChannel::EventMessage& event);
    void ShowGroup(uint32_t screen_group_id);
    void ShowOverlay(uint32_t screen_id);
    void CloseOverlay();
    void ConfigureGestures();

    // TODO: For test purposes only
    void SetupTestConfiguration();
    void SetupTestAssets();

public:
    UiController(
        std::shared_ptr<IFsService> fs_service,
        std::shared_ptr<WorkQueueThread> config_work_queue_thread,
        std::shared_ptr<ConfigurationService> configuration_service,
        std::shared_ptr<SensorReadingsFrame> sensor_readings_frame);
    ~UiController();

    int Initialize();
    int Start();

    std::shared_ptr<NavigationService> GetNavigationService() const;
};

} // namespace eerie_leap::controllers
