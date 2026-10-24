#pragma once

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include <lvgl.h>

#include "domain/ui_domain/models/icon_type.h"
#include "domain/ui_domain/models/widget_configuration.h"
#include "subsys/assets/asset_kind.h"
#include "subsys/assets/asset_provider.h"
#include "subsys/assets/asset_store.h"
#include "subsys/assets/assets_manager.h"
#include "views/renderable_base.h"
#include "views/screens/i_screen.h"
#include "views/utilitites/frame.h"
#include "views/widgets/i_widget.h"
#include "views/widgets/widget_context.h"

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

// The bytes of a text, as the asset stores take them.
std::span<const uint8_t> TextBytes(std::string_view text);

// One row of an ARGB8888 snapshot or canvas buffer.
const lv_color32_t* PixelRow(const lv_draw_buf_t& buffer, uint32_t row);

// A store for a directory of the internal volume, which is mounted on first use. Files a test saved
// there stay for the tests after it.
std::shared_ptr<eerie_leap::subsys::assets::AssetStore> MakeAssetStore(std::string_view directory);

// A registry that serves the store as the kind, the way UiController registers its kinds, with the kind's built-in
// files if it has any. Only IMAGES, ANIMATIONS and FONTS are supported.
std::shared_ptr<eerie_leap::subsys::assets::AssetsManager> MakeAssetsManager(
    eerie_leap::subsys::assets::AssetKind kind,
    std::shared_ptr<eerie_leap::subsys::assets::AssetStore> store,
    std::span<const eerie_leap::subsys::assets::BuiltInAsset> built_ins = {}
);

// A fill-slot BasicIcon needle for a dial. Labels get text so they can render.
std::shared_ptr<eerie_leap::domain::ui_domain::models::WidgetConfiguration> NeedleConfiguration(
    uint32_t id,
    eerie_leap::domain::ui_domain::models::IconType type = eerie_leap::domain::ui_domain::models::IconType::Label
);

// Builds each child through the factory under the owner's mount, records their IDs
// on the owner's configuration in order, and injects them. Configure the owner next.
void InjectChildren(
    eerie_leap::views::widgets::IWidget& owner,
    eerie_leap::domain::ui_domain::models::WidgetConfiguration& owner_configuration,
    std::initializer_list<std::shared_ptr<eerie_leap::domain::ui_domain::models::WidgetConfiguration>> children,
    const eerie_leap::views::widgets::WidgetContext& context = {}
);

// The real Screen pulls in a widget tree, a grid layout and a configuration;
// the group only ever calls the interface below.
class FakeScreen : public eerie_leap::views::RenderableBase, public eerie_leap::views::screens::IScreen {
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
        std::shared_ptr<eerie_leap::views::utilitites::Frame> parent
    );

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

    void ApplyWidgetPatches(
        std::shared_ptr<eerie_leap::domain::ui_domain::models::ScreenConfiguration> configuration,
        std::span<const eerie_leap::domain::ui_domain::preview::WidgetPatch> patches
    ) override;
};

} // namespace views_test
