#include <algorithm>
#include <memory_resource>
#include <span>
#include <utility>

#include "domain/ui_domain/lvgl_lock.h"
#include "domain/ui_domain/models/widget_composition.h"
#include "views/utilitites/grid_layout.h"
#include "views/widgets/widget_factory.h"

#include "widget_assembly.h"

namespace eerie_leap::views::screens {

using eerie_leap::domain::ui_domain::ScopedLvglLock;
using eerie_leap::domain::ui_domain::models::WidgetComposition;
using eerie_leap::views::utilitites::GridLayout;
using eerie_leap::views::widgets::WidgetFactory;

WidgetAssembly::Roots WidgetAssembly::Assemble(
    const std::shared_ptr<ScreenConfiguration>& configuration,
    const std::shared_ptr<Frame>& container,
    const WidgetContext& context
) {
    return AssembleRoots(configuration, std::nullopt, container, context);
}

std::unique_ptr<IWidget> WidgetAssembly::AssembleRoot(
    const std::shared_ptr<ScreenConfiguration>& configuration,
    uint32_t root_id,
    const std::shared_ptr<Frame>& container,
    const WidgetContext& context
) {
    auto roots = AssembleRoots(configuration, root_id, container, context);
    if(roots.empty())
        return nullptr;

    return std::move(roots.front());
}

WidgetAssembly::Roots WidgetAssembly::AssembleRoots(
    const std::shared_ptr<ScreenConfiguration>& configuration,
    std::optional<uint32_t> only_root_id,
    const std::shared_ptr<Frame>& container,
    const WidgetContext& context
) {
    ScopedLvglLock lvgl_guard;

    const auto& definitions = configuration->widget_configurations;
    auto* resource = definitions.get_allocator().resource();
    auto& factory = WidgetFactory::GetInstance();
    const auto layout = GridLayout::FromActiveScreen(configuration->grid);
    const auto composition = WidgetComposition::Build(*configuration);

    // The roots to build, and everything they own.
    std::pmr::vector<size_t> roots(resource);
    for(const auto i : composition.roots) {
        if(!only_root_id.has_value() || definitions[i]->id == *only_root_id)
            roots.push_back(i);
    }

    std::pmr::vector<bool> included(definitions.size(), false, resource);
    for(const auto i : roots)
        included[i] = true;
    // A postorder lists descendants before owners, so walking it backwards visits every owner first.
    for(auto it = composition.postorder.rbegin(); it != composition.postorder.rend(); ++it) {
        if(!included[*it])
            continue;
        for(const auto child : composition.GetChildren(*it))
            included[child] = true;
    }

    std::pmr::vector<std::unique_ptr<IWidget>> instances(definitions.size(), resource);
    std::pmr::vector<size_t> siblings(resource);
    siblings.reserve(definitions.size());

    const auto construct = [&](std::span<const size_t> indices, const std::shared_ptr<Frame>& mount) {
        siblings.assign(indices.begin(), indices.end());
        // Creation order is sibling drawing order; semantic child order is untouched.
        std::ranges::sort(siblings, {}, [&](size_t i) {
            return std::pair{ definitions[i]->z_index, i };
        });
        for(const auto i : siblings)
            instances[i] = factory.CreateWidget(definitions[i]->type, definitions[i]->id, mount, context);
    };

    try {
        construct(roots, container);
        // Before configuring, which registers the box's properties on a root.
        for(const auto i : roots)
            instances[i]->AttachGrid(layout);
        // Reverse postorder visits every owner before its descendants.
        for(auto it = composition.postorder.rbegin(); it != composition.postorder.rend(); ++it) {
            if(!included[*it])
                continue;
            const auto children = composition.GetChildren(*it);
            if(!children.empty())
                construct(children, instances[*it]->GetChildMount());
        }

        for(const auto i : composition.postorder) {
            if(!included[i])
                continue;
            const auto indices = composition.GetChildren(i);
            IWidget::Children children;
            children.reserve(indices.size());
            for(const auto child : indices)
                children.push_back(std::move(instances[child]));

            WidgetFactory::ConfigureWidget(*instances[i], definitions[i], std::move(children));
        }

        for(const auto i : roots) {
            const auto size_px = layout.ToPx(definitions[i]->size_grid);
            instances[i]->SetSizePx(size_px);
            instances[i]->SetPositionPx(layout.ToPx(definitions[i]->position_grid, size_px));
        }
    } catch(...) {
        // An owner's LVGL deletion would take unowned descendants' objects with it.
        for(const auto i : composition.postorder)
            instances[i].reset();
        throw;
    }

    Roots built;
    built.reserve(roots.size());
    for(const auto i : roots)
        built.push_back(std::move(instances[i]));

    return built;
}

} // namespace eerie_leap::views::screens
