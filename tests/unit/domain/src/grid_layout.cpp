#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>

#include <zephyr/ztest.h>

#include "domain/ui_domain/models/grid_settings.h"
#include "domain/ui_domain/models/widget_position.h"
#include "domain/ui_domain/models/widget_size.h"
#include "views/utilitites/grid_layout.h"

using eerie_leap::domain::ui_domain::models::GridSettings;
using eerie_leap::domain::ui_domain::models::WidgetPosition;
using eerie_leap::domain::ui_domain::models::WidgetSize;
using eerie_leap::views::utilitites::GridLayout;

namespace {

constexpr int32_t SCREEN_SIZE = 400;

GridSettings MakeGrid(uint32_t width, uint32_t height, uint32_t spacing_px) {
    return GridSettings{ .snap_enabled = false, .width = width, .height = height, .spacing_px = spacing_px };
}

// GridLayout::FromActiveScreen() needs a bound display, so the tests build the
// layout against a fixed screen size instead.
GridLayout MakeLayout(uint32_t grid_width, uint32_t grid_height, uint32_t spacing_px) {
    return { MakeGrid(grid_width, grid_height, spacing_px), SCREEN_SIZE, SCREEN_SIZE };
}

std::string InvalidArgumentMessage(const GridSettings& grid, int32_t screen_width, int32_t screen_height) {
    try {
        GridLayout layout(grid, screen_width, screen_height);
    } catch(const std::invalid_argument& e) {
        return e.what();
    } catch(...) {
        return "unexpected exception";
    }

    return "no exception";
}

template<typename Fn>
std::string ThrownMessage(Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch(const std::exception& e) {
        return e.what();
    } catch(...) {
        return "unexpected exception";
    }

    return "no exception";
}

} // namespace

ZTEST_SUITE(grid_layout, NULL, NULL, NULL, NULL, NULL);

ZTEST(grid_layout, test_a_widget_spanning_the_whole_grid_fills_the_screen) {
    auto layout = MakeLayout(4, 4, 5);

    auto size_px = layout.ToPx(WidgetSize{ .width = 4, .height = 4 });

    // The full-span shortcut ignores the spacing so the widget reaches the edges.
    zassert_equal(size_px.width, static_cast<uint32_t>(SCREEN_SIZE));
    zassert_equal(size_px.height, static_cast<uint32_t>(SCREEN_SIZE));
}

ZTEST(grid_layout, test_a_cell_sized_widget_is_one_grid_step_without_spacing) {
    auto layout = MakeLayout(4, 4, 0);

    auto size_px = layout.ToPx(WidgetSize{ .width = 1, .height = 1 });

    zassert_equal(size_px.width, 100U);
    zassert_equal(size_px.height, 100U);
}

ZTEST(grid_layout, test_the_spacing_is_removed_from_both_edges_of_a_widget) {
    auto layout = MakeLayout(4, 4, 5);

    auto size_px = layout.ToPx(WidgetSize{ .width = 1, .height = 1 });

    // 400 / 4 - 5 * 2
    zassert_equal(size_px.width, 90U);
    zassert_equal(size_px.height, 90U);
}

ZTEST(grid_layout, test_a_widget_scales_with_the_number_of_cells_it_spans) {
    auto layout = MakeLayout(4, 4, 0);

    auto size_px = layout.ToPx(WidgetSize{ .width = 2, .height = 3 });

    zassert_equal(size_px.width, 200U);
    zassert_equal(size_px.height, 300U);
}

ZTEST(grid_layout, test_each_axis_uses_its_own_grid_extent) {
    GridLayout layout(MakeGrid(2, 8, 0), SCREEN_SIZE, SCREEN_SIZE);

    auto size_px = layout.ToPx(WidgetSize{ .width = 1, .height = 1 });

    zassert_equal(size_px.width, 200U);
    zassert_equal(size_px.height, 50U);
}

ZTEST(grid_layout, test_the_origin_sits_at_the_bottom_left_of_the_screen) {
    auto layout = MakeLayout(4, 4, 0);
    WidgetSize size_px{ .width = 100, .height = 100 };

    auto position_px = layout.ToPx(WidgetPosition{ .x = 0, .y = 0 }, size_px);

    // Grid y counts up from the bottom, pixel y counts down from the top.
    zassert_equal(position_px.x, 0);
    zassert_equal(position_px.y, 300);
}

ZTEST(grid_layout, test_the_top_row_lands_flush_with_the_top_edge) {
    auto layout = MakeLayout(4, 4, 0);
    WidgetSize size_px{ .width = 100, .height = 100 };

    auto position_px = layout.ToPx(WidgetPosition{ .x = 1, .y = 3 }, size_px);

    zassert_equal(position_px.x, 100);
    zassert_equal(position_px.y, 0);
}

ZTEST(grid_layout, test_a_position_advances_by_whole_cells) {
    auto layout = MakeLayout(4, 4, 0);
    WidgetSize size_px{ .width = 100, .height = 100 };

    auto position_px = layout.ToPx(WidgetPosition{ .x = 3, .y = 1 }, size_px);

    zassert_equal(position_px.x, 300);
    zassert_equal(position_px.y, 200);
}

ZTEST(grid_layout, test_the_spacing_widens_the_cell_step_of_a_position) {
    auto layout = MakeLayout(4, 4, 5);
    WidgetSize size_px{ .width = 90, .height = 90 };

    // Cell step is 400 / 4 + 5 * 2.
    auto position_px = layout.ToPx(WidgetPosition{ .x = 2, .y = 2 }, size_px);

    zassert_equal(position_px.x, 220);
    zassert_equal(position_px.y, 90);
}

ZTEST(grid_layout, test_a_grid_wider_than_the_screen_is_rejected) {
    zassert_equal(
        InvalidArgumentMessage(MakeGrid(SCREEN_SIZE + 1, 4, 0), SCREEN_SIZE, SCREEN_SIZE),
        std::string("Invalid screen width.")
    );
}

ZTEST(grid_layout, test_a_grid_taller_than_the_screen_is_rejected) {
    zassert_equal(
        InvalidArgumentMessage(MakeGrid(4, SCREEN_SIZE + 1, 0), SCREEN_SIZE, SCREEN_SIZE),
        std::string("Invalid screen height.")
    );
}

ZTEST(grid_layout, test_an_empty_grid_axis_is_rejected) {
    zassert_equal(
        InvalidArgumentMessage(MakeGrid(0, 4, 0), SCREEN_SIZE, SCREEN_SIZE), std::string("Invalid screen width.")
    );

    zassert_equal(
        InvalidArgumentMessage(MakeGrid(4, 0, 0), SCREEN_SIZE, SCREEN_SIZE), std::string("Invalid screen height.")
    );
}

ZTEST(grid_layout, test_an_unsized_screen_is_rejected) {
    zassert_equal(
        InvalidArgumentMessage(MakeGrid(4, 4, 0), 0, SCREEN_SIZE), std::string("Invalid active screen size.")
    );

    zassert_equal(
        InvalidArgumentMessage(MakeGrid(4, 4, 0), SCREEN_SIZE, -1), std::string("Invalid active screen size.")
    );
}

ZTEST(grid_layout, test_a_widget_without_an_extent_is_rejected) {
    auto layout = MakeLayout(4, 4, 0);

    zassert_equal(
        ThrownMessage([&layout] {
            (void)layout.ToPx(WidgetSize{ .width = 0, .height = 1 });
        }),
        std::string("Invalid widget width.")
    );

    zassert_equal(
        ThrownMessage([&layout] {
            (void)layout.ToPx(WidgetSize{ .width = 1, .height = 0 });
        }),
        std::string("Invalid widget height.")
    );
}

ZTEST(grid_layout, test_positioning_a_widget_of_unknown_size_is_rejected) {
    auto layout = MakeLayout(4, 4, 0);
    WidgetPosition position_grid{ .x = 0, .y = 0 };

    zassert_equal(
        ThrownMessage([&layout, &position_grid] {
            (void)layout.ToPx(position_grid, WidgetSize{ .width = 0, .height = 0 });
        }),
        std::string("Widget size is not set.")
    );
}
