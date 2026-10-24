#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

#include <lvgl.h>
#include <zephyr/logging/log.h>

#include "domain/ui_domain/lvgl_lock.h"
#include "domain/ui_domain/models/widget_composition.h"
#include "views/screens/widget_assembly.h"

#include "screen.h"

namespace eerie_leap::views::screens {

using namespace eerie_leap::views::widgets;
using eerie_leap::domain::ui_domain::ScopedLvglLock;
using eerie_leap::domain::ui_domain::models::WidgetComposition;
using eerie_leap::domain::ui_domain::preview::WidgetAction;

LOG_MODULE_REGISTER(screen_logger);

Screen::Screen(uint32_t id, const std::shared_ptr<Frame>& parent, WidgetContext context) :
    id_(id),
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
    // A failed widget stays unready; failing the screen would keep its whole group hidden.
    for(auto& widget : *widgets_) {
        try {
            if(widget->Render() != 0)
                LOG_ERR("Failed to render widget %u on screen %u.", widget->GetId(), id_);
        } catch(const std::exception& e) {
            LOG_ERR("Failed to render widget %u on screen %u. %s", widget->GetId(), id_, e.what());
        }
    }

    return 0;
}

int Screen::ApplyTheme(const ITheme& /*theme*/) {
    return 0;
}

void Screen::Configure(std::shared_ptr<ScreenConfiguration> configuration) {
    ScopedLvglLock lvgl_guard;

    *widgets_ = WidgetAssembly::Assemble(configuration, container_, context_);
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
        lv_obj_set_hidden(container_->GetObject(), false);
    else
        lv_obj_set_hidden(container_->GetObject(), true);
}

void Screen::OnActivated() {
    ScopedLvglLock lvgl_guard;
    is_activated_ = true;
    for(auto& widget : *widgets_)
        widget->OnActivated();
}

void Screen::OnDeactivated() {
    ScopedLvglLock lvgl_guard;
    is_activated_ = false;
    for(auto& widget : *widgets_)
        widget->OnDeactivated();
}

void Screen::ApplyWidgetPatches(
    std::shared_ptr<ScreenConfiguration> configuration,
    std::span<const WidgetPatch> patches
) {
    ScopedLvglLock lvgl_guard;

    auto& roots = *widgets_;
    const auto find = [&roots](uint32_t id) {
        return std::ranges::find_if(roots, [id](const auto& widget) {
            return widget->GetId() == id;
        });
    };

    // What goes first goes whole, before anything is built in the places it leaves.
    for(const auto& patch : patches) {
        if(patch.action != WidgetAction::Remove)
            continue;

        const auto it = find(patch.widget_id);
        if(it == roots.end())
            continue;

        (*it)->OnDeactivated();
        roots.erase(it);
    }

    for(const auto& patch : patches) {
        if(patch.action == WidgetAction::Remove)
            continue;

        if(patch.action == WidgetAction::Update) {
            if(const auto it = find(patch.widget_id); it != roots.end())
                (*it)->ApplyEditorConfiguration(patch.widget);

            continue;
        }

        // The successor stands before the widget it replaces goes: a failure to build leaves the old one.
        auto successor = WidgetAssembly::AssembleRoot(configuration, patch.widget_id, container_, context_);
        if(successor == nullptr)
            throw std::logic_error("A patched widget is not a root of its screen.");

        if(is_ready_) {
            try {
                if(successor->Render() != 0)
                    LOG_ERR("Failed to render widget %u on screen %u.", patch.widget_id, id_);
            } catch(const std::exception& e) {
                LOG_ERR("Failed to render widget %u on screen %u. %s", patch.widget_id, id_, e.what());
            }
        }

        if(is_activated_)
            successor->OnActivated();

        const auto it = find(patch.widget_id);
        if(it != roots.end()) {
            (*it)->OnDeactivated();
            std::swap(*it, successor);
        } else {
            roots.push_back(std::move(successor));
        }
    }

    configuration_ = std::move(configuration);
    ApplyRootOrder();
}

void Screen::ApplyRootOrder() {
    const auto composition = WidgetComposition::Build(*configuration_);
    const auto& definitions = configuration_->widget_configurations;

    std::vector<size_t> order(composition.roots.begin(), composition.roots.end());
    std::ranges::sort(order, {}, [&definitions](size_t i) {
        return std::pair{ definitions[i]->z_index, i };
    });

    auto& roots = *widgets_;
    std::vector<std::unique_ptr<IWidget>> ordered;
    ordered.reserve(roots.size());
    for(const auto i : order) {
        const auto it = std::ranges::find_if(roots, [&](const auto& widget) {
            return widget != nullptr && widget->GetId() == definitions[i]->id;
        });
        if(it != roots.end())
            ordered.push_back(std::move(*it));
    }

    // A root the configuration does not list has no place; it should not exist.
    for(auto& widget : roots) {
        if(widget != nullptr)
            ordered.push_back(std::move(widget));
    }

    roots = std::move(ordered);

    int32_t index = 0;
    for(const auto& widget : roots) {
        const auto container = widget->GetContainer();
        if(container != nullptr && container->GetObject() != nullptr)
            lv_obj_move_to_index(container->GetObject(), index++);
    }
}

std::shared_ptr<std::vector<std::unique_ptr<IWidget>>> Screen::GetWidgets() const {
    return widgets_;
}

} // namespace eerie_leap::views::screens
