#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <eerie_memory.hpp>
#include <lvgl.h>
#include <zephyr/ztest.h>

#include "utilities/memory/memory_resource_manager.h"

#include "domain/ui_domain/configuration/parsers/ui_configuration_validator.h"
#include "domain/ui_domain/models/icon_type.h"
#include "domain/ui_domain/models/ui_configuration.h"
#include "domain/ui_domain/preview/ui_patch.h"
#include "domain/ui_domain/utilities/widget_property_validator.h"
#include "views/main_view.h"
#include "views/screens/screen.h"
#include "views/screens/screen_group.h"
#include "views/themes/theme_manager.h"
#include "views/utilities/lvgl_color.h"
#include "views/utilitites/frame.h"
#include "views/utilitites/grid_layout.h"
#include "views/widgets/widget_base.h"
#include "views/widgets/widget_factory.h"

#include "views_test_support.h"

using namespace eerie_memory;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::domain::ui_domain::preview;
using namespace eerie_leap::views::widgets;

using eerie_leap::domain::ui_domain::configuration::parsers::UiConfigurationValidator;
using eerie_leap::domain::ui_domain::utilities::WidgetPropertyValidator;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::views::MainView;
using eerie_leap::views::screens::Screen;
using eerie_leap::views::screens::ScreenGroup;
using eerie_leap::views::themes::ThemeManager;
using eerie_leap::views::utilities::LvglColor;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::utilitites::GridLayout;
using views_test::CleanTestDisplay;
using views_test::EnsureTestDisplay;
using views_test::FakeScreen;
using views_test::WidgetContent;

namespace {

constexpr uint32_t GRID_CELLS = 10;

std::shared_ptr<Frame> MakeContainer() {
    return std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(100, false).SetHeight(100, false).Build());
}

std::shared_ptr<WidgetConfiguration> Bar(uint32_t id, int32_t z_index, int32_t x) {
    auto bar = make_shared_pmr<WidgetConfiguration>(Mrm::GetExtPmr());
    bar->type = WidgetType::IndicatorBar;
    bar->id = id;
    bar->position_grid = { x, 0 };
    bar->size_grid = { 2, 2 };
    bar->z_index = z_index;

    return bar;
}

std::shared_ptr<WidgetConfiguration> Copy(const WidgetConfiguration& source) {
    auto widget = make_shared_pmr<WidgetConfiguration>(Mrm::GetExtPmr());
    widget->type = source.type;
    widget->id = source.id;
    widget->position_grid = source.position_grid;
    widget->size_grid = source.size_grid;
    widget->z_index = source.z_index;
    for(const auto& [type, value] : source.properties)
        widget->properties.emplace(type, value);
    widget->bindings.assign(source.bindings.begin(), source.bindings.end());

    return widget;
}

std::shared_ptr<ScreenConfiguration> MakeScreen(uint32_t id = 1, uint32_t group_id = 1) {
    auto screen = make_shared_pmr<ScreenConfiguration>(Mrm::GetExtPmr());
    screen->id = id;
    screen->screen_group_id = group_id;
    screen->type = ScreenType::Gauge;
    screen->grid = { .snap_enabled = true, .width = GRID_CELLS, .height = GRID_CELLS, .spacing_px = 0 };

    return screen;
}

void Validate(const ScreenConfiguration& screen) {
    UiConfigurationValidator::Validate(screen, [](const auto& owner, auto children) {
        WidgetFactory::GetInstance().ValidateChildren(owner, children);
    });
}

void CheckBackground(lv_obj_t* object, lv_part_t part, const LvglColor& expected) {
    zassert_equal(lv_color_to_u32(lv_obj_get_style_bg_color(object, part)), lv_color_to_u32(expected.ToLvColor()));
    zassert_equal(lv_obj_get_style_bg_opa(object, part), expected.ToLvOpa());
}

lv_obj_t* Inner(const IWidget& widget) {
    return lv_obj_get_child(WidgetContent(widget), 0);
}

std::vector<uint32_t> Ids(const Screen& screen) {
    std::vector<uint32_t> ids;
    for(const auto& widget : *screen.GetWidgets())
        ids.push_back(widget->GetId());

    return ids;
}

IWidget* Find(const Screen& screen, uint32_t id) {
    for(const auto& widget : *screen.GetWidgets()) {
        if(widget->GetId() == id)
            return widget.get();
    }

    return nullptr;
}

// A screen that is rendered and active, patched as a preview patches it.
struct Scene {
    std::shared_ptr<Frame> container = MakeContainer();
    Screen screen{ 1, container, WidgetContext{} };
    std::shared_ptr<ScreenConfiguration> configuration;

    explicit Scene(std::shared_ptr<ScreenConfiguration> initial) :
        configuration(std::move(initial)) {
        Validate(*configuration);
        screen.Configure(configuration);
        zassert_equal(screen.Render(), 0);
        screen.OnActivated();
    }

    PatchPlan Patch(const std::vector<PatchOp>& ops) {
        auto base = make_shared_pmr<UiConfiguration>(Mrm::GetExtPmr());
        base->screen_configurations.push_back(configuration);

        const auto patched = ApplyPatchOps(*base, ops, Mrm::GetExtPmr());
        Validate(*patched->screen_configurations.front());

        auto plan = PlanPatch(*base, *patched, ops);
        zassert_equal(plan.screens.size(), 1U);
        zassert_true(plan.screens[0].action == ScreenAction::Widgets);

        configuration = plan.screens[0].new_screen;
        screen.ApplyWidgetPatches(configuration, plan.screens[0].widgets);

        return plan;
    }
};

PatchOp Put(const std::shared_ptr<WidgetConfiguration>& widget) {
    return { .kind = PatchOpKind::PutWidget, .screen_id = 1, .widget = widget };
}

PatchOp Remove(uint32_t widget_id) {
    return { .kind = PatchOpKind::RemoveWidget, .screen_id = 1, .widget_id = widget_id };
}

void* SetUp() {
    EnsureTestDisplay();

    return nullptr;
}

} // namespace

ZTEST_SUITE(widget_patch, NULL, SetUp, NULL, CleanTestDisplay, NULL);

// The point of an in-place patch: the same widget object and the same LVGL objects, taking the new values.
ZTEST(widget_patch, test_an_in_place_change_keeps_the_objects_and_orders_the_roots_by_z_index) {
    auto first = Bar(1, 0, 0);
    auto second = Bar(2, 1, 4);
    auto screen_configuration = MakeScreen();
    screen_configuration->AddWidget(first);
    screen_configuration->AddWidget(second);
    Scene scene(screen_configuration);

    auto* before = Find(scene.screen, 1);
    auto* other = Find(scene.screen, 2);
    lv_obj_t* const container = before->GetContainer()->GetObject();
    lv_obj_t* const content = WidgetContent(*before);
    lv_obj_t* const inner = Inner(*before);
    const auto child_count = lv_obj_get_child_count(scene.screen.GetContainer()->GetObject());
    zassert_true((Ids(scene.screen) == std::vector<uint32_t>{ 1, 2 }));
    zassert_equal(lv_obj_get_index(container), 0);

    auto recolored = Copy(*first);
    recolored->properties[WidgetPropertyType::COLOR_PRIMARY_ACTIVE] = std::pmr::string("#FF0000FF");
    recolored->properties[WidgetPropertyType::OPACITY] = 128;
    recolored->properties[WidgetPropertyType::MAX_VALUE] = 200.0;
    recolored->position_grid = { 5, 3 };
    recolored->size_grid = { 3, 3 };
    recolored->z_index = 3;
    const auto plan = scene.Patch({ Put(recolored) });

    zassert_true(plan.effects[0] == PatchEffect::InPlace);
    zassert_equal(Find(scene.screen, 1), before, "The widget is the one it was");
    zassert_equal(Find(scene.screen, 2), other);
    zassert_equal(before->GetContainer()->GetObject(), container);
    zassert_equal(WidgetContent(*before), content);
    zassert_equal(Inner(*before), inner);
    zassert_equal(lv_obj_get_child_count(scene.screen.GetContainer()->GetObject()), child_count);
    zassert_equal(before->GetConfiguration(), recolored);

    // What the new record says, shown.
    lv_obj_update_layout(scene.screen.GetContainer()->GetObject());
    CheckBackground(inner, LV_PART_INDICATOR, LvglColor(0xFF0000, 255));
    zassert_equal(lv_obj_get_style_opa_layered(container, LV_PART_MAIN), 128);
    const auto layout = GridLayout::FromActiveScreen(scene.configuration->grid);
    const auto size = layout.ToPx(recolored->size_grid);
    zassert_equal(before->GetSizePx().width, size.width);
    zassert_equal(before->GetSizePx().height, size.height);
    zassert_equal(before->GetPositionPx().x, layout.ToPx(recolored->position_grid, size).x);
    zassert_equal(before->GetPositionPx().y, layout.ToPx(recolored->position_grid, size).y);

    lv_obj_update_layout(scene.screen.GetContainer()->GetObject());
    zassert_equal(lv_obj_get_x(container), layout.ToPx(recolored->position_grid, size).x, "The LVGL object moved");
    zassert_equal(lv_obj_get_width(container), static_cast<int32_t>(size.width));

    // The z-index puts it above the other one: the vector and LVGL's children agree.
    zassert_true((Ids(scene.screen) == std::vector<uint32_t>{ 2, 1 }));
    zassert_equal(lv_obj_get_index(other->GetContainer()->GetObject()), 0);
    zassert_equal(lv_obj_get_index(container), 1);
}

ZTEST(widget_patch, test_a_property_the_record_drops_goes_back_to_its_default) {
    auto bar = Bar(1, 0, 0);
    bar->properties[WidgetPropertyType::COLOR_PRIMARY_ACTIVE] = std::pmr::string("#33CC99FF");
    bar->properties[WidgetPropertyType::OPACITY] = 100;
    auto screen_configuration = MakeScreen();
    screen_configuration->AddWidget(bar);
    Scene scene(screen_configuration);

    auto* widget = Find(scene.screen, 1);
    lv_obj_t* const container = widget->GetContainer()->GetObject();
    lv_obj_t* const inner = Inner(*widget);
    CheckBackground(inner, LV_PART_INDICATOR, LvglColor(0x33CC99, 255));
    zassert_equal(lv_obj_get_style_opa_layered(container, LV_PART_MAIN), 100);

    scene.Patch({ Put(Bar(1, 0, 0)) });

    zassert_equal(Find(scene.screen, 1), widget);
    CheckBackground(inner, LV_PART_INDICATOR, ThemeManager::GetInstance().GetCurrentTheme().GetSecondaryColor());
    zassert_equal(lv_obj_get_style_opa_layered(container, LV_PART_MAIN), 255);

    // Hidden, and shown again by the record that leaves it out.
    auto hidden = Bar(1, 0, 0);
    hidden->properties[WidgetPropertyType::IS_VISIBLE] = false;
    scene.Patch({ Put(hidden) });
    zassert_true(lv_obj_is_hidden(container));

    scene.Patch({ Put(Bar(1, 0, 0)) });
    zassert_false(lv_obj_is_hidden(container));
    zassert_equal(Find(scene.screen, 1), widget);
}

ZTEST(widget_patch, test_a_change_that_needs_a_rebuild_replaces_the_widget_in_its_place) {
    auto first = Bar(1, 0, 0);
    auto second = Bar(2, 1, 4);
    auto third = Bar(3, 2, 6);
    auto screen_configuration = MakeScreen();
    screen_configuration->AddWidget(first);
    screen_configuration->AddWidget(second);
    screen_configuration->AddWidget(third);
    Scene scene(screen_configuration);

    auto* old_widget = Find(scene.screen, 2);
    lv_obj_t* const old_container = old_widget->GetContainer()->GetObject();
    const auto child_count = lv_obj_get_child_count(scene.screen.GetContainer()->GetObject());

    // A smoothed bar is built smoothed.
    auto smoothed = Copy(*second);
    smoothed->properties[WidgetPropertyType::IS_SMOOTHED] = true;
    const auto plan = scene.Patch({ Put(smoothed) });

    zassert_true(plan.effects[0] == PatchEffect::WidgetRebuilt);
    auto* new_widget = Find(scene.screen, 2);
    zassert_not_null(new_widget);
    zassert_not_equal(new_widget->GetContainer()->GetObject(), old_container, "A new widget");
    zassert_true(new_widget->IsReady(), "Built, rendered");
    zassert_true(new_widget->IsSmoothed());
    zassert_equal(new_widget->GetConfiguration(), smoothed);
    zassert_equal(lv_obj_get_child_count(scene.screen.GetContainer()->GetObject()), child_count, "and the old one gone");

    // Between its neighbours, as before, in the vector and in LVGL.
    zassert_true((Ids(scene.screen) == std::vector<uint32_t>{ 1, 2, 3 }));
    zassert_equal(lv_obj_get_index(Find(scene.screen, 1)->GetContainer()->GetObject()), 0);
    zassert_equal(lv_obj_get_index(new_widget->GetContainer()->GetObject()), 1);
    zassert_equal(lv_obj_get_index(Find(scene.screen, 3)->GetContainer()->GetObject()), 2);
}

ZTEST(widget_patch, test_widgets_are_added_and_removed_and_the_rest_stays) {
    auto first = Bar(1, 0, 0);
    auto second = Bar(2, 2, 4);
    auto screen_configuration = MakeScreen();
    screen_configuration->AddWidget(first);
    screen_configuration->AddWidget(second);
    Scene scene(screen_configuration);
    auto* kept = Find(scene.screen, 2);
    lv_obj_t* const kept_container = kept->GetContainer()->GetObject();

    const auto plan = scene.Patch({ Remove(1), Put(Bar(3, 1, 6)), Put(Bar(4, 3, 8)) });

    zassert_equal(plan.effects.size(), 3U);
    for(const auto effect : plan.effects)
        zassert_true(effect == PatchEffect::WidgetRebuilt);

    zassert_true((Ids(scene.screen) == std::vector<uint32_t>{ 3, 2, 4 }));
    zassert_equal(Find(scene.screen, 2), kept);
    zassert_equal(kept->GetContainer()->GetObject(), kept_container);
    zassert_equal(lv_obj_get_child_count(scene.screen.GetContainer()->GetObject()), 3U);
    for(uint32_t index = 0; index < 3; ++index)
        zassert_equal(lv_obj_get_index((*scene.screen.GetWidgets())[index]->GetContainer()->GetObject()), index);

    zassert_true(Find(scene.screen, 3)->IsReady());
    zassert_true(Find(scene.screen, 4)->IsReady());
}

ZTEST(widget_patch, test_what_changes_in_a_child_rebuilds_its_owner) {
    auto dial = make_shared_pmr<WidgetConfiguration>(Mrm::GetExtPmr());
    dial->type = WidgetType::IndicatorDial;
    dial->id = 20;
    dial->position_grid = { 0, 0 };
    dial->size_grid = { 6, 6 };
    dial->properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::pmr::vector<int>({ 21 }, Mrm::GetExtPmr());
    auto needle = views_test::NeedleConfiguration(21, IconType::Label);
    auto screen_configuration = MakeScreen();
    screen_configuration->AddWidget(dial);
    screen_configuration->AddWidget(needle);
    screen_configuration->AddWidget(Bar(30, 1, 7));
    Scene scene(screen_configuration);

    auto* old_dial = Find(scene.screen, 20);
    auto* bar = Find(scene.screen, 30);
    zassert_not_null(old_dial);
    zassert_equal(old_dial->GetChildren().size(), 1U);

    // An in-place change for a root, but this one is the dial's.
    auto relabelled = Copy(*needle);
    relabelled->properties[WidgetPropertyType::COLOR_PRIMARY_ACTIVE] = std::pmr::string("#FF0000FF");
    const auto plan = scene.Patch({ Put(relabelled) });

    zassert_true(plan.effects[0] == PatchEffect::WidgetRebuilt);
    zassert_equal(plan.screens[0].widgets.size(), 1U);
    zassert_equal(plan.screens[0].widgets[0].widget_id, 20U);
    auto* new_dial = Find(scene.screen, 20);
    zassert_not_null(new_dial);
    zassert_equal(new_dial->GetChildren().size(), 1U);
    zassert_equal(new_dial->GetChildren()[0]->GetConfiguration(), relabelled);
    zassert_true(new_dial->IsReady());
    zassert_equal(Find(scene.screen, 30), bar, "A widget it does not own is not touched");
    zassert_true((Ids(scene.screen) == std::vector<uint32_t>{ 20, 30 }));
}

ZTEST(widget_patch, test_a_record_of_another_widget_is_refused) {
    auto screen_configuration = MakeScreen();
    screen_configuration->AddWidget(Bar(1, 0, 0));
    Scene scene(screen_configuration);
    auto* widget = Find(scene.screen, 1);

    bool is_refused = false;
    try {
        widget->ApplyEditorConfiguration(Bar(2, 0, 0));
    } catch(const std::logic_error&) {
        is_refused = true;
    }
    zassert_true(is_refused, "Another widget's ID");

    is_refused = false;
    auto digital = Bar(1, 0, 0);
    digital->type = WidgetType::IndicatorDigital;
    try {
        widget->ApplyEditorConfiguration(digital);
    } catch(const std::logic_error&) {
        is_refused = true;
    }
    zassert_true(is_refused, "Another type");
    zassert_not_equal(widget->GetConfiguration(), digital);
}

// ======== Screens in groups ========

ZTEST(widget_patch, test_a_screen_put_in_a_rendered_active_group_takes_the_place_of_its_namesake) {
    auto root = MakeContainer();
    auto group = std::make_shared<ScreenGroup>(0, root);
    auto first = std::make_shared<FakeScreen>(1, 0, 0, true, group->GetContainer());
    auto second = std::make_shared<FakeScreen>(2, 0, 1, true, group->GetContainer());
    group->AddScreen(first);
    group->AddScreen(second);
    zassert_equal(group->EnsureRendered(), 0);
    group->Activate();

    auto successor = std::make_shared<FakeScreen>(1, 0, 2, true, group->GetContainer());
    group->PutScreen(successor);

    zassert_equal(group->GetScreen(1), successor);
    zassert_equal(group->GetScreens().size(), 2U);
    zassert_equal(successor->render_count, 1, "Rendered by the group that is");
    zassert_equal(successor->activated_count, 1, "and activated by the group that is");
    zassert_equal(first->deactivated_count, 1);
    // Above the other one now, as its z-index says.
    zassert_equal(lv_obj_get_index(second->GetContainer()->GetObject()), 0);
    zassert_equal(lv_obj_get_index(successor->GetContainer()->GetObject()), 1);

    auto added = std::make_shared<FakeScreen>(3, 0, 0, true, group->GetContainer());
    group->PutScreen(added);
    zassert_equal(group->GetScreens().size(), 3U);
    zassert_equal(added->render_count, 1);
    zassert_equal(lv_obj_get_index(added->GetContainer()->GetObject()), 0);

    zassert_true(group->RemoveScreen(2));
    zassert_false(group->RemoveScreen(2));
    zassert_equal(group->GetScreens().size(), 2U);
    zassert_equal(second->deactivated_count, 1);
}

ZTEST(widget_patch, test_a_group_that_is_not_rendered_renders_a_put_screen_later) {
    auto root = MakeContainer();
    auto group = std::make_shared<ScreenGroup>(0, root);
    auto first = std::make_shared<FakeScreen>(1, 0, 0, true, group->GetContainer());
    group->AddScreen(first);

    auto successor = std::make_shared<FakeScreen>(1, 0, 0, true, group->GetContainer());
    group->PutScreen(successor);

    zassert_equal(successor->render_count, 0);
    zassert_equal(group->EnsureRendered(), 0);
    zassert_equal(successor->render_count, 1);
    zassert_equal(first->render_count, 0);
}

ZTEST(widget_patch, test_the_view_puts_a_screen_in_its_group_and_keeps_the_last_screen_of_a_group) {
    MainView view;
    auto first = std::make_shared<FakeScreen>(1, 0, 0, true, view.GetGroupContainer(0));
    auto other_group = std::make_shared<FakeScreen>(2, 1, 0, true, view.GetGroupContainer(1));
    view.AddScreen(first);
    view.AddScreen(other_group);
    zassert_equal(view.SetActiveGroup(0), 0);
    zassert_equal(view.Render(), 0);

    auto successor = std::make_shared<FakeScreen>(1, 0, 0, true, view.GetGroupContainer(0));
    zassert_true(view.PutScreen(successor));
    zassert_equal(view.GetScreen(1), successor);
    zassert_equal(successor->activated_count, 1);

    // A group the view does not have, and a screen that is in another group.
    zassert_false(view.PutScreen(std::make_shared<FakeScreen>(5, 9, 0, true, view.GetGroupContainer(0))));
    zassert_false(view.PutScreen(std::make_shared<FakeScreen>(2, 0, 0, true, view.GetGroupContainer(0))));

    // The only screen of a group stays: a group with none is no group.
    zassert_false(view.RemoveScreen(1));
    zassert_false(view.RemoveScreen(99));
    auto second = std::make_shared<FakeScreen>(3, 0, 1, true, view.GetGroupContainer(0));
    zassert_true(view.PutScreen(second));
    zassert_true(view.RemoveScreen(1));
    zassert_is_null(view.GetScreen(1).get());
    zassert_equal(view.GetScreen(3), second);
}

// ======== The classifier and the widgets ========

// The classifier is a table, and the widgets are what it describes: a property a widget registers as needing a
// rebuild is never one it calls in place.
ZTEST(widget_patch, test_the_classifier_never_applies_in_place_what_a_widget_registers_as_a_rebuild) {
    struct Case {
        WidgetType type;
        IconType icon;
    };
    const std::vector<Case> cases{
        { WidgetType::IndicatorArcFill, IconType::None },
        { WidgetType::IndicatorSegmentArc, IconType::None },
        { WidgetType::IndicatorDigital, IconType::None },
        { WidgetType::IndicatorHorizontalChart, IconType::None },
        { WidgetType::IndicatorDial, IconType::None },
        { WidgetType::IndicatorBar, IconType::None },
        { WidgetType::IndicatorSetting, IconType::None },
        { WidgetType::ControlSlider, IconType::None },
        { WidgetType::ControlToggle, IconType::None },
        { WidgetType::ControlButton, IconType::None },
        { WidgetType::BasicLottie, IconType::None },
        { WidgetType::BasicIcon, IconType::Label },
        { WidgetType::BasicIcon, IconType::Image },
        { WidgetType::BasicIcon, IconType::Rectangle },
        { WidgetType::BasicIcon, IconType::TriangleIsosceles },
        { WidgetType::BasicIcon, IconType::TriangleRight },
        { WidgetType::BasicIcon, IconType::Oval },
        { WidgetType::BasicIcon, IconType::Line },
        { WidgetType::BasicArcIcon, IconType::Rectangle },
    };

    auto container = MakeContainer();
    auto& factory = WidgetFactory::GetInstance();
    const WidgetContext context{};

    for(const auto& [type, icon] : cases) {
        auto configuration = make_shared_pmr<WidgetConfiguration>(Mrm::GetExtPmr());
        configuration->type = type;
        configuration->id = 1;
        configuration->size_grid = { 2, 2 };
        if(icon != IconType::None)
            configuration->properties[WidgetPropertyType::ICON_TYPE] = static_cast<int>(icon);

        std::unique_ptr<IWidget> widget;
        if(type == WidgetType::IndicatorDial) {
            widget = factory.CreateWidget(type, 1, container, context);
            views_test::InjectChildren(*widget, *configuration, { views_test::NeedleConfiguration(2, IconType::Label) }, context);
            widget->Configure(configuration);
        } else {
            widget = factory.CreateWidget(configuration, container, context);
        }

        const auto& base = static_cast<const WidgetBase&>(*widget);
        size_t compared = 0;
        for(const auto property : widget->GetSupportedProperties()) {
            if(WidgetPropertyValidator::IsLayoutProperty(property) || WidgetPropertyValidator::IsStructuralProperty(property))
                continue;

            auto changed = Copy(*configuration);
            // A value no record has, so the property differs whether the record had it or not.
            changed->properties[property] = 987654;
            ++compared;

            if(ClassifyWidgetChange(*configuration, *changed) != WidgetChange::InPlace)
                continue;

            zassert_not_equal(
                base.GetPropertyEffect(property), PropertyChangeEffect::Rebuild,
                "Widget type %u, icon %u: property %u is registered as a rebuild but applied in place",
                static_cast<unsigned>(type), static_cast<unsigned>(icon), static_cast<unsigned>(property)
            );
        }

        zassert_true(compared > 0, "Widget type %u registered nothing", static_cast<unsigned>(type));
    }
}

ZTEST(widget_patch, test_screens_are_put_back_in_the_order_a_whole_stage_would_give) {
    auto root = MakeContainer();
    auto group = std::make_shared<ScreenGroup>(0, root);
    auto first = std::make_shared<FakeScreen>(1, 0, 0, true, group->GetContainer());
    auto second = std::make_shared<FakeScreen>(2, 0, 0, true, group->GetContainer());
    auto third = std::make_shared<FakeScreen>(3, 0, 0, true, group->GetContainer());
    group->AddScreen(first);
    group->AddScreen(second);
    group->AddScreen(third);
    zassert_equal(group->EnsureRendered(), 0);
    group->Activate();

    const auto ids = [&group] {
        std::vector<uint32_t> result;
        for(const auto& screen : group->GetScreens())
            result.push_back(screen->GetId());

        return result;
    };
    zassert_true((ids() == std::vector<uint32_t>{ 1, 2, 3 }));

    // A put inserts above the screens of its z-index; the configuration still lists it first.
    auto again = std::make_shared<FakeScreen>(1, 0, 0, true, group->GetContainer());
    group->PutScreen(again);
    zassert_true((ids() == std::vector<uint32_t>{ 2, 3, 1 }));

    const std::array<uint32_t, 3> order{ 1, 2, 3 };
    group->OrderScreens(order);
    zassert_true((ids() == std::vector<uint32_t>{ 1, 2, 3 }));
    zassert_equal(lv_obj_get_index(again->GetContainer()->GetObject()), 0);
    zassert_equal(lv_obj_get_index(second->GetContainer()->GetObject()), 1);
    zassert_equal(lv_obj_get_index(third->GetContainer()->GetObject()), 2);

    // A screen the order does not name follows the ones it does.
    const std::array<uint32_t, 2> partial{ 3, 2 };
    group->OrderScreens(partial);
    zassert_true((ids() == std::vector<uint32_t>{ 3, 2, 1 }));
}

// What the three kinds of change cost on this host, printed for the record (the unit's CPU is slower by a factor the
// report estimates). The numbers are not asserted; the ordering is.
ZTEST(widget_patch, test_an_in_place_change_costs_less_than_a_widget_rebuild_and_a_widget_less_than_a_screen) {
    constexpr int ROUNDS = 20;
    constexpr uint32_t WIDGETS = 12;

    auto screen_configuration = MakeScreen();
    for(uint32_t id = 0; id < WIDGETS; ++id)
        screen_configuration->AddWidget(Bar(id, static_cast<int32_t>(id), static_cast<int32_t>(id) % 8));
    Scene scene(screen_configuration);

    const auto measure = [](auto&& work) {
        const auto start = std::chrono::steady_clock::now();
        for(int round = 0; round < ROUNDS; ++round)
            work(round);

        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / ROUNDS;
    };

    const auto in_place = measure([&scene](int round) {
        auto bar = Copy(*Find(scene.screen, 3)->GetConfiguration());
        bar->properties[WidgetPropertyType::OPACITY] = 100 + round;
        bar->position_grid.x = round % 8;
        scene.Patch({ Put(bar) });
    });
    const auto rebuilt = measure([&scene](int round) {
        auto bar = Copy(*Find(scene.screen, 3)->GetConfiguration());
        bar->properties[WidgetPropertyType::IS_SMOOTHED] = round % 2 == 0;
        scene.Patch({ Put(bar) });
    });
    const auto screen = measure([&scene](int round) {
        auto copy = MakeScreen();
        for(const auto& widget : scene.configuration->widget_configurations)
            copy->AddWidget(Copy(*widget));
        copy->grid.width = GRID_CELLS + static_cast<uint32_t>(round % 2);
        Screen rebuilt_screen(7, scene.container, WidgetContext{});
        rebuilt_screen.Configure(copy);
        (void)rebuilt_screen.Render();
    });

    printk("PATCH COST (host, %u widgets on the screen): in place %u us, one widget rebuilt %u us, a screen built again %u us\n", WIDGETS, static_cast<unsigned>(in_place), static_cast<unsigned>(rebuilt), static_cast<unsigned>(screen));
    zassert_true(in_place < rebuilt, "in place %u us, rebuilt %u us", static_cast<unsigned>(in_place), static_cast<unsigned>(rebuilt));
    zassert_true(rebuilt < screen, "rebuilt %u us, screen %u us", static_cast<unsigned>(rebuilt), static_cast<unsigned>(screen));
}
