#include <array>
#include <cerrno>
#include <span>
#include <utility>

#include <lvgl.h>
#include <zephyr/ztest.h>

#include "subsys/assets/asset_provider.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "views/widgets/widget_factory.h"

#include "views_test_support.h"

namespace views_test {

using eerie_leap::domain::ui_domain::models::IconType;
using eerie_leap::domain::ui_domain::models::ScreenConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetPropertyType;
using eerie_leap::domain::ui_domain::models::WidgetType;
using eerie_leap::subsys::assets::AssetAccess;
using eerie_leap::subsys::assets::AssetKind;
using eerie_leap::subsys::assets::AssetProvider;
using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::subsys::assets::AssetStore;
using eerie_leap::subsys::assets::BuiltInAsset;
using eerie_leap::subsys::device_tree::DtFs;
using eerie_leap::subsys::fs::services::FsService;
using eerie_leap::views::themes::ITheme;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::widgets::IWidget;
using eerie_leap::views::widgets::WidgetContext;
using eerie_leap::views::widgets::WidgetFactory;

namespace {

constexpr int32_t DISPLAY_WIDTH = 466;
constexpr int32_t DISPLAY_HEIGHT = 466;
constexpr int32_t BUFFER_LINES = 10;
constexpr int32_t MAX_BYTES_PER_PIXEL = 4;

// A provider keeps only a span of its extensions.
constexpr std::array<std::string_view, 1> IMAGE_EXTENSIONS{ ".bin" };
constexpr std::array<std::string_view, 1> ANIMATION_EXTENSIONS{ ".json" };
constexpr std::array<std::string_view, 1> FONT_EXTENSIONS{ ".ttf" };

uint8_t draw_buffer[DISPLAY_WIDTH * BUFFER_LINES * MAX_BYTES_PER_PIXEL];
lv_display_t* test_display = nullptr;

void FlushCb(lv_display_t* display, const lv_area_t* /*area*/, uint8_t* /*px_map*/) {
    lv_display_flush_ready(display);
}

} // namespace

void EnsureTestDisplay() {
    if(!lv_is_initialized())
        lv_init();

    if(test_display != nullptr)
        return;

    test_display = lv_display_create(DISPLAY_WIDTH, DISPLAY_HEIGHT);
    lv_display_set_buffers(test_display, draw_buffer, nullptr, sizeof(draw_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(test_display, FlushCb);
}

void CleanTestDisplay(void* /*fixture*/) {
    if(test_display != nullptr)
        lv_obj_clean(lv_screen_active());
}

lv_obj_t* WidgetContent(const IWidget& widget) {
    return widget.GetContainer()->GetChild()->GetObject();
}

std::span<const uint8_t> TextBytes(std::string_view text) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): characters viewed as the bytes they are.
    return { reinterpret_cast<const uint8_t*>(text.data()), text.size() };
}

const lv_color32_t* PixelRow(const lv_draw_buf_t& buffer, uint32_t row) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): an LVGL draw buffer is a byte array of pixels.
    return reinterpret_cast<const lv_color32_t*>(buffer.data + (row * buffer.header.stride));
}

std::shared_ptr<AssetStore> MakeAssetStore(std::string_view directory) {
    DtFs::InitInternalFs();
    auto fs = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    zassert_true(fs->Initialize());

    return std::make_shared<AssetStore>(std::move(fs), directory);
}

std::shared_ptr<AssetsManager>
MakeAssetsManager(AssetKind kind, std::shared_ptr<AssetStore> store, std::span<const BuiltInAsset> built_ins) {
    zassert_true(kind == AssetKind::IMAGES || kind == AssetKind::ANIMATIONS || kind == AssetKind::FONTS);
    const bool is_image = kind == AssetKind::IMAGES;
    const bool is_font = kind == AssetKind::FONTS;

    auto assets = std::make_shared<AssetsManager>();
    zassert_true(assets->Register(
        AssetProvider{ .kind = kind,
            .name = is_image ? "images" : (is_font ? "fonts" : "animations"),
            .store = std::move(store),
            .extensions = is_image ? IMAGE_EXTENSIONS : (is_font ? FONT_EXTENSIONS : ANIMATION_EXTENSIONS),
            .access = static_cast<uint8_t>(
                ToMask(AssetAccess::READ) | ToMask(AssetAccess::WRITE) | ToMask(AssetAccess::DELETE)
            ),
            .max_size = 1024 * 1024,
            .built_ins = built_ins }
    ));

    return assets;
}

std::shared_ptr<WidgetConfiguration> NeedleConfiguration(uint32_t id, IconType type) {
    auto needle = std::make_shared<WidgetConfiguration>(std::allocator_arg, std::pmr::get_default_resource());
    needle->id = id;
    needle->type = WidgetType::BasicIcon;
    needle->position_grid = { 0, 0 };
    needle->size_grid = { 1, 1 };
    needle->properties[WidgetPropertyType::ICON_TYPE] = static_cast<int>(type);
    if(type == IconType::Label)
        needle->properties[WidgetPropertyType::LABEL] = std::pmr::string("|");
    return needle;
}

void InjectChildren(
    IWidget& owner,
    WidgetConfiguration& owner_configuration,
    std::initializer_list<std::shared_ptr<WidgetConfiguration>> children,
    const WidgetContext& context
) {

    auto& factory = WidgetFactory::GetInstance();
    std::pmr::vector<int> ids(owner_configuration.properties.get_allocator().resource());
    IWidget::Children widgets;
    for(const auto& child : children) {
        ids.push_back(static_cast<int>(child->id));
        widgets.push_back(factory.CreateWidget(child, owner.GetChildMount(), context));
    }
    owner_configuration.properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::move(ids);
    owner.SetChildren(std::move(widgets));
}

FakeScreen::FakeScreen(
    uint32_t id,
    uint32_t screen_group_id,
    int32_t z_index,
    bool is_visible,
    std::shared_ptr<Frame> parent
) :
    id_(id),
    screen_group_id_(screen_group_id),
    z_index_(z_index),
    is_visible_(is_visible),
    parent_(std::move(parent)) {}

void FakeScreen::FailNextRenders() {
    fails_to_render_ = true;
}

int FakeScreen::DoRender() {
    render_count++;

    if(fails_to_render_)
        return -EIO;

    container_ = std::make_shared<Frame>(
        Frame::CreateWrapped(parent_->GetObject()).SetWidth(100, false).SetHeight(100, false).Build()
    );

    return 0;
}

int FakeScreen::ApplyTheme(const ITheme& /*theme*/) {
    return 0;
}

void FakeScreen::Configure(std::shared_ptr<ScreenConfiguration> configuration) {}

std::shared_ptr<ScreenConfiguration> FakeScreen::GetConfiguration() const {
    return nullptr;
}

std::shared_ptr<std::vector<std::unique_ptr<IWidget>>> FakeScreen::GetWidgets() const {
    return nullptr;
}

uint32_t FakeScreen::GetId() const {
    return id_;
}

uint32_t FakeScreen::GetGroupId() const {
    return screen_group_id_;
}

int32_t FakeScreen::GetZIndex() const {
    return z_index_;
}

bool FakeScreen::IsVisible() const {
    return is_visible_;
}

void FakeScreen::ApplyWidgetPatches(
    std::shared_ptr<ScreenConfiguration> /*configuration*/,
    std::span<const eerie_leap::domain::ui_domain::preview::WidgetPatch> /*patches*/
) {}

void FakeScreen::OnActivated() {
    activated_count++;
}

void FakeScreen::OnDeactivated() {
    deactivated_count++;
}

} // namespace views_test
