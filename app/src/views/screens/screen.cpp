#include <stdexcept>

#include <zephyr/logging/log.h>
#include <lvgl.h>

#include "domain/ui_domain/lvgl_lock.h"
#include "views/widgets/widget_factory.h"

#include "screen.h"

namespace eerie_leap::views::screens {

using namespace eerie_leap::views::widgets;
using eerie_leap::domain::ui_domain::ScopedLvglLock;

LOG_MODULE_REGISTER(screen_logger);

Screen::Screen(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context)
        : id_(id),
        parent_(parent),
        context_(std::move(context)) {

    widgets_ = std::make_shared<std::vector<std::unique_ptr<IWidget>>>();

    container_ = std::make_shared<Frame>(Frame::CreateWrapped(parent->GetObject())
        .SetProcessingParent(parent)
        .SetWidth(100, false)
        .SetHeight(100, false)
        .Build());
}

int Screen::DoRender() {
    for(auto& widget : *widgets_)
        widget->Render();

    return 0;
}

int Screen::ApplyTheme(const ITheme& theme) {
    return 0;
}

void Screen::Configure(std::shared_ptr<ScreenConfiguration> configuration) {
    configuration_ = std::move(configuration);

    auto layout = GridLayout::FromActiveScreen(configuration_->grid);

    SetVisibility(IsVisible());

    widgets_->clear();

    for(const auto& widget_config : configuration_->widget_configurations) {
        try {
            auto widget = WidgetFactory::GetInstance().CreateWidget(widget_config, container_, context_);
            UpdateWidgetGeometry(*widget, layout);

            widgets_->push_back(std::move(widget));
        } catch(const std::exception& e) {
            LOG_ERR("Failed to create widget with ID: %d. %s", widget_config->id, e.what());
        }
    }
}

std::shared_ptr<ScreenConfiguration> Screen::GetConfiguration() const {
    return configuration_;
}

uint32_t Screen::GetId() const {
    return id_;
}

uint32_t Screen::GetGroupId() const {
    return configuration_ != nullptr ? configuration_->screen_group_id : 0;
}

int32_t Screen::GetZIndex() const {
    return configuration_ != nullptr ? configuration_->z_index : 0;
}

bool Screen::IsVisible() const {
    return configuration_ == nullptr || configuration_->is_visible;
}

void Screen::SetVisibility(bool is_visible) {
    ScopedLvglLock lvgl_guard;
    if(is_visible)
        lv_obj_remove_flag(container_->GetObject(), LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(container_->GetObject(), LV_OBJ_FLAG_HIDDEN);
}

void Screen::OnActivated() {
    ScopedLvglLock lvgl_guard;
    for(auto& widget : *widgets_)
        widget->OnActivated();
}

void Screen::OnDeactivated() {
    ScopedLvglLock lvgl_guard;
    for(auto& widget : *widgets_)
        widget->OnDeactivated();
}

std::shared_ptr<std::vector<std::unique_ptr<IWidget>>> Screen::GetWidgets() const {
    return widgets_;
}

void Screen::UpdateWidgetGeometry(IWidget& widget, const GridLayout& layout) {
    const auto& widget_config = widget.GetConfiguration();

    auto size_px = layout.ToPx(widget_config->size_grid);
    widget.SetSizePx(size_px);
    widget.SetPositionPx(layout.ToPx(widget_config->position_grid, size_px));
}

} // namespace eerie_leap::views::screens
