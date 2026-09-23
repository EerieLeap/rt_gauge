#include <unordered_map>

#include "screen_configuration.h"
#include "widget_composition.h"

namespace eerie_leap::domain::ui_domain::models {

WidgetComposition WidgetComposition::Build(const ScreenConfiguration& configuration) {
    const auto& definitions = configuration.widget_configurations;
    auto* resource = definitions.get_allocator().resource();
    WidgetComposition composition(resource);
    composition.nodes.resize(definitions.size());
    composition.roots.reserve(definitions.size());
    composition.postorder.reserve(definitions.size());
    std::pmr::unordered_map<uint32_t, size_t> indices(resource);
    indices.reserve(definitions.size());
    size_t edge_count = 0;

    for(size_t i = 0; i < definitions.size(); ++i) {
        indices.emplace(definitions[i]->id, i);
        const auto it = definitions[i]->properties.find(WidgetPropertyType::CHILD_WIDGET_IDS);
        if(it != definitions[i]->properties.end())
            edge_count += std::get<std::pmr::vector<int>>(it->second).size();
    }
    composition.children.reserve(edge_count);

    for(size_t i = 0; i < definitions.size(); ++i) {
        auto& node = composition.nodes[i];
        node.children_begin = composition.children.size();
        const auto it = definitions[i]->properties.find(WidgetPropertyType::CHILD_WIDGET_IDS);
        if(it == definitions[i]->properties.end())
            continue;
        const auto& ids = std::get<std::pmr::vector<int>>(it->second);
        node.children_count = ids.size();
        for(int id : ids) {
            const auto child = indices.at(static_cast<uint32_t>(id));
            composition.nodes[child].parent = i;
            composition.children.push_back(child);
        }
    }
    for(size_t i = 0; i < definitions.size(); ++i) {
        if(!composition.nodes[i].parent.has_value())
            composition.roots.push_back(i);
    }

    struct Visit { size_t index; size_t next_child; };
    std::pmr::vector<Visit> stack(resource);
    stack.reserve(max_depth);
    for(auto root : composition.roots) {
        stack.push_back({ root, 0 });
        while(!stack.empty()) {
            auto& current = stack.back();
            const auto children = composition.GetChildren(current.index);
            if(current.next_child < children.size()) {
                const auto child = children[current.next_child++];
                stack.push_back({ child, 0 });
            } else {
                composition.postorder.push_back(current.index);
                stack.pop_back();
            }
        }
    }

    return composition;
}

} // namespace eerie_leap::domain::ui_domain::models
