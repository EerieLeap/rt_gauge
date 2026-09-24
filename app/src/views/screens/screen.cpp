#include <zephyr/logging/log.h>
#include <lvgl.h>

#include "domain/ui_domain/lvgl_lock.h"

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
    // Render every root so one failed asset does not hide the rest; report the first failure.
    int result = 0;
    for(auto& widget : *widgets_) {
        const int res = widget->Render();
        if(res != 0) {
            LOG_ERR("Failed to render widget %u on screen %u.", widget->GetId(), id_);
            if(result == 0)
                result = res;
        }
    }

    return result;
}

int Screen::ApplyTheme(const ITheme& theme) {
    return 0;
}

void Screen::Configure(std::shared_ptr<ScreenConfiguration> configuration) {
    ScopedLvglLock lvgl_guard;

    auto assembly = WidgetAssembly::Assemble(configuration, container_, context_);
    assembly.Commit();

    // Replacing the assembly destroys the previous tree only after the new one exists.
    assembly_ = std::move(assembly);
    widgets_ = assembly_->GetRoots();
    configuration_ = std::move(configuration);

    SetVisibility(IsVisible());
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

} // namespace eerie_leap::views::screens
