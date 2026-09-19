#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <lvgl.h>

#include "views/renderable_base.h"
#include "views/screens/i_screen.h"
#include "views/utilitites/frame.h"
#include "views/widgets/i_widget.h"

namespace views_test {

// LVGL is built with CONFIG_LV_Z_AUTO_INIT=n in the test suites, so there is no
// display bound and lv_screen_active() would be null. A buffer-only display is
// enough for everything these tests assert: object trees, flags and child order.
void EnsureTestDisplay();

// Frames do not own their lv_obj_t, so a test's objects outlive it as orphaned
// children of the active screen.
void CleanTestDisplay(void* fixture);

// The stable presentation object beneath a widget's public layout container.
lv_obj_t* WidgetContent(const eerie_leap::views::widgets::IWidget& widget);

// The real Screen pulls in a widget tree, a grid layout and a configuration;
// the group only ever calls the interface below.
class FakeScreen : public eerie_leap::views::RenderableBase,
                   public eerie_leap::views::screens::IScreen {
private:
    uint32_t id_;
    uint32_t screen_group_id_;
    int32_t z_index_;
    bool is_visible_;
    std::shared_ptr<eerie_leap::views::utilitites::Frame> parent_;
    bool fails_to_render_ = false;

    int DoRender() override;
    int ApplyTheme(const eerie_leap::views::themes::ITheme& theme) override;

public:
    FakeScreen(
        uint32_t id,
        uint32_t screen_group_id,
        int32_t z_index,
        bool is_visible,
        std::shared_ptr<eerie_leap::views::utilitites::Frame> parent);

    int render_count = 0;
    int activated_count = 0;
    int deactivated_count = 0;

    void FailNextRenders();

    void Configure(std::shared_ptr<eerie_leap::domain::ui_domain::models::ScreenConfiguration> configuration) override;
    std::shared_ptr<eerie_leap::domain::ui_domain::models::ScreenConfiguration> GetConfiguration() const override;
    std::shared_ptr<std::vector<std::unique_ptr<eerie_leap::views::widgets::IWidget>>> GetWidgets() const override;

    uint32_t GetId() const override;
    uint32_t GetGroupId() const override;
    int32_t GetZIndex() const override;
    bool IsVisible() const override;

    void OnActivated() override;
    void OnDeactivated() override;
};

} // namespace views_test
