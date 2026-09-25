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
    std::shared_ptr<ScreenConfiguration> configuration,
    std::shared_ptr<Frame> container,
    const WidgetContext& context
) {
    ScopedLvglLock lvgl_guard;

    const auto& definitions = configuration->widget_configurations;
    auto* resource = definitions.get_allocator().resource();
    auto& factory = WidgetFactory::GetInstance();
    const auto layout = GridLayout::FromActiveScreen(configuration->grid);
    const auto composition = WidgetComposition::Build(*configuration);

    std::pmr::vector<std::unique_ptr<IWidget>> instances(definitions.size(), resource);
    std::pmr::vector<size_t> siblings(resource);
    siblings.reserve(definitions.size());

    auto construct = [&](std::span<const size_t> indices, const std::shared_ptr<Frame>& mount) {
        siblings.assign(indices.begin(), indices.end());
        // Creation order is sibling drawing order; semantic child order is untouched.
        std::ranges::sort(siblings, {}, [&](size_t i) { return std::pair { definitions[i]->z_index, i }; });
        for(auto i : siblings)
            instances[i] = factory.CreateWidget(definitions[i]->type, definitions[i]->id, mount, context);
    };

    try {
        construct(composition.roots, container);
        // Reverse postorder visits every owner before its descendants.
        for(auto it = composition.postorder.rbegin(); it != composition.postorder.rend(); ++it) {
            const auto children = composition.GetChildren(*it);
            if(!children.empty())
                construct(children, instances[*it]->GetChildMount());
        }

        for(auto i : composition.postorder) {
            const auto indices = composition.GetChildren(i);
            IWidget::Children children;
            children.reserve(indices.size());
            for(auto child : indices)
                children.push_back(std::move(instances[child]));

            factory.ConfigureWidget(*instances[i], definitions[i], std::move(children));
        }

        for(auto i : composition.roots) {
            const auto size_px = layout.ToPx(definitions[i]->size_grid);
            instances[i]->SetSizePx(size_px);
            instances[i]->SetPositionPx(layout.ToPx(definitions[i]->position_grid, size_px));
        }
    } catch(...) {
        // An owner's LVGL deletion would take unowned descendants' objects with it.
        for(auto i : composition.postorder)
            instances[i].reset();
        throw;
    }

    Roots roots;
    roots.reserve(composition.roots.size());
    for(auto i : composition.roots)
        roots.push_back(std::move(instances[i]));

    return roots;
}

} // namespace eerie_leap::views::screens
