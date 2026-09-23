#pragma once

#include <cstddef>
#include <memory_resource>
#include <optional>
#include <span>
#include <vector>

namespace eerie_leap::domain::ui_domain::models {

struct ScreenConfiguration;

// Indices refer to ScreenConfiguration::widget_configurations, which must remain
// unchanged while consuming this plan. Child order is semantic, not drawing order.
struct WidgetComposition {
    static constexpr size_t max_nodes = 32;
    static constexpr size_t max_edges = 31;
    static constexpr size_t max_depth = 8;

    struct Node {
        std::optional<size_t> parent;
        size_t children_begin = 0;
        size_t children_count = 0;
    };

    std::pmr::vector<Node> nodes;
    std::pmr::vector<size_t> children;
    std::pmr::vector<size_t> roots;
    // Descendants before owners; roots and each owner's children retain definition/reference order.
    std::pmr::vector<size_t> postorder;

    // Requires a screen that passed UiConfigurationValidator::Validate and has
    // not changed since. Builds relationships only; it does not repeat validation.
    static WidgetComposition Build(const ScreenConfiguration& configuration);

    explicit WidgetComposition(std::pmr::memory_resource* resource)
        : nodes(resource), children(resource), roots(resource), postorder(resource) {}

    WidgetComposition(const WidgetComposition&) = delete;
    WidgetComposition& operator=(const WidgetComposition&) = delete;
    WidgetComposition(WidgetComposition&&) noexcept = default;
    WidgetComposition& operator=(WidgetComposition&&) = default;

    std::span<const size_t> GetChildren(size_t index) const {
        const auto& node = nodes.at(index);
        return std::span<const size_t>(children).subspan(node.children_begin, node.children_count);
    }
};

} // namespace eerie_leap::domain::ui_domain::models
