#include <cerrno>
#include <utility>

#include <lvgl.h>

#include "views/widgets/widget_factory.h"

#include "views_test_support.h"

namespace views_test {

using eerie_leap::domain::ui_domain::models::IconType;
using eerie_leap::domain::ui_domain::models::ScreenConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetPropertyType;
using eerie_leap::domain::ui_domain::models::WidgetType;
using eerie_leap::views::themes::ITheme;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::widgets::IWidget;
using eerie_leap::views::widgets::WidgetContext;
using eerie_leap::views::widgets::WidgetFactory;

namespace {

constexpr int32_t display_width = 466;
constexpr int32_t display_height = 466;
constexpr int32_t buffer_lines = 10;
constexpr int32_t max_bytes_per_pixel = 4;

uint8_t draw_buffer[display_width * buffer_lines * max_bytes_per_pixel];
lv_display_t* test_display = nullptr;

void FlushCb(lv_display_t* display, const lv_area_t* area, uint8_t* px_map) {
    lv_display_flush_ready(display);
}

} // namespace

void EnsureTestDisplay() {
    if(!lv_is_initialized())
        lv_init();

    if(test_display != nullptr)
        return;

    test_display = lv_display_create(display_width, display_height);
    lv_display_set_buffers(
        test_display,
        draw_buffer,
        nullptr,
        sizeof(draw_buffer),
        LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(test_display, FlushCb);
}

void CleanTestDisplay(void* fixture) {
    if(test_display != nullptr)
        lv_obj_clean(lv_screen_active());
}

lv_obj_t* WidgetContent(const IWidget& widget) {
    return widget.GetContainer()->GetChild()->GetObject();
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
    const WidgetContext& context) {

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
    std::shared_ptr<Frame> parent)
    : id_(id),
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

    container_ = std::make_shared<Frame>(Frame::CreateWrapped(parent_->GetObject())
        .SetWidth(100, false)
        .SetHeight(100, false)
        .Build());

    return 0;
}

int FakeScreen::ApplyTheme(const ITheme& theme) {
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

void FakeScreen::OnActivated() {
    activated_count++;
}

void FakeScreen::OnDeactivated() {
    deactivated_count++;
}

} // namespace views_test
