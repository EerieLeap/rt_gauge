#pragma once

#include <algorithm>
#include <memory>
#include <memory_resource>
#include <vector>

#include "grid_settings.h"
#include "screen_type.h"
#include "widget_configuration.h"

namespace eerie_leap::domain::ui_domain::models {

struct ScreenConfiguration {
    using allocator_type = std::pmr::polymorphic_allocator<>;

    uint32_t id;
    uint32_t screen_group_id;
    ScreenType type;
    int32_t z_index = 0;
    bool is_visible = true;

    // An overlay is hosted by OverlayHost on the top layer instead of joining a screen group.
    bool is_overlay = false;
    GridSettings grid;
    std::pmr::vector<std::shared_ptr<WidgetConfiguration>> widget_configurations;

    ScreenConfiguration(std::allocator_arg_t, allocator_type alloc) :
        widget_configurations(alloc) {}

    ScreenConfiguration(const ScreenConfiguration&) = delete;
    ScreenConfiguration& operator=(const ScreenConfiguration&) = delete;
    ScreenConfiguration& operator=(ScreenConfiguration&&) noexcept = default;
    ScreenConfiguration(ScreenConfiguration&&) noexcept = default;
    ~ScreenConfiguration() = default;

    ScreenConfiguration(ScreenConfiguration&& other, allocator_type alloc) :
        id(other.id),
        screen_group_id(other.screen_group_id),
        type(other.type),
        z_index(other.z_index),
        is_visible(other.is_visible),
        is_overlay(other.is_overlay),
        grid(other.grid),
        widget_configurations(std::move(other.widget_configurations), alloc) {}

    void AddWidget(std::shared_ptr<WidgetConfiguration> widget_configuration) {
        const auto insert_position = std::upper_bound(
            widget_configurations.begin(), widget_configurations.end(), widget_configuration->z_index,
            [](int32_t z_index, const std::shared_ptr<WidgetConfiguration>& current) {
                return z_index < current->z_index;
            }
        );

        widget_configurations.insert(insert_position, std::move(widget_configuration));
    }
};

} // namespace eerie_leap::domain::ui_domain::models
