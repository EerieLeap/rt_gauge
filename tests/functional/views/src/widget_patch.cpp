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

#include "domain/sensor_domain/event_bus/sensor_events_channel.h"
#include "domain/ui_domain/configuration/parsers/ui_configuration_validator.h"
#include "domain/ui_domain/models/icon_type.h"
#include "domain/ui_domain/models/ui_configuration.h"
#include "domain/ui_domain/preview/ui_patch.h"
#include "domain/ui_domain/utilities/widget_property_validator.h"
#include "event_bus/event_channels.h"
#include "subsys/event_bus/event_channel_id.h"
#include "utilities/string/string_helpers.h"
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

using namespace eerie_leap::domain::sensor_domain::event_bus;
using eerie_leap::domain::ui_domain::configuration::parsers::UiConfigurationValidator;
using eerie_leap::domain::ui_domain::utilities::WidgetPropertyValidator;
using eerie_leap::event_bus::InitializeEventChannels;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::utilities::memory::Mrm;
using eerie_leap::utilities::string::StringHelpers;
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
    InitializeEventChannels();

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
    zassert_equal(
        lv_obj_get_child_count(scene.screen.GetContainer()->GetObject()), child_count, "and the old one gone"
    );

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
        zassert_equal(
            lv_obj_get_index((*scene.screen.GetWidgets())[index]->GetContainer()->GetObject()),
            static_cast<int32_t>(index)
        );

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
            views_test::InjectChildren(
                *widget, *configuration, { views_test::NeedleConfiguration(2, IconType::Label) }, context
            );
            widget->Configure(configuration);
        } else {
            widget = factory.CreateWidget(configuration, container, context);
        }

        const auto& base = dynamic_cast<const WidgetBase&>(*widget);
        size_t compared = 0;
        for(const auto property : widget->GetSupportedProperties()) {
            if(WidgetPropertyValidator::IsLayoutProperty(property)
                || WidgetPropertyValidator::IsStructuralProperty(property))
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
// report estimates). A clock is no proof of anything, so nothing here is asserted.
ZTEST(widget_patch, test_what_an_in_place_change_a_widget_rebuild_and_a_screen_rebuild_cost) {
    constexpr int ROUNDS = 20;
    constexpr uint32_t WIDGETS = 12;

    auto screen_configuration = MakeScreen();
    for(uint32_t id = 0; id < WIDGETS; ++id)
        screen_configuration->AddWidget(Bar(id, static_cast<int32_t>(id), static_cast<int32_t>(id) % 8));
    Scene scene(screen_configuration);

    // native_sim's clock stands still while code runs, so there the host's clock times it. Elsewhere the kernel's
    // does: the std::chrono clocks of the Zephyr SDK's libstdc++ need gettimeofday, which picolibc does not have.
    const auto now_us = [] -> int64_t {
#ifdef CONFIG_ARCH_POSIX
        const auto since_epoch = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::microseconds>(since_epoch).count();
#else
        return static_cast<int64_t>(k_ticks_to_us_floor64(k_uptime_ticks()));
#endif
    };
    const auto measure = [&now_us](auto&& work) {
        const int64_t start = now_us();
        for(int round = 0; round < ROUNDS; ++round)
            work(round);

        return static_cast<double>(now_us() - start) / ROUNDS;
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

    printk(
        "PATCH COST (host, %u widgets on the screen): in place %u us, one widget rebuilt %u us, a screen built again "
        "%u us\n",
        WIDGETS, static_cast<unsigned>(in_place), static_cast<unsigned>(rebuilt), static_cast<unsigned>(screen)
    );
}

// ======== A widget patched in place looks as one built from the new record ========

namespace {

struct LookCase {
    WidgetType type;
    IconType icon;
};

// A test table of a few cases; the build has no use for a way to catch its construction failing.
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const std::vector<LookCase> LOOK_CASES{
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
    // (No lottie: it cannot be built without an asset, and the table lists nothing of its own for it.)
    { WidgetType::BasicIcon, IconType::Label },
    { WidgetType::BasicIcon, IconType::Image },
    { WidgetType::BasicIcon, IconType::Rectangle },
    { WidgetType::BasicIcon, IconType::TriangleIsosceles },
    { WidgetType::BasicIcon, IconType::TriangleRight },
    { WidgetType::BasicIcon, IconType::Oval },
    { WidgetType::BasicIcon, IconType::Line },
    { WidgetType::BasicArcIcon, IconType::Rectangle },
};

// The records of a widget under test, the widget first: a dial has a needle.
std::vector<std::shared_ptr<WidgetConfiguration>> LookRecords(const LookCase& look) {
    auto root = make_shared_pmr<WidgetConfiguration>(Mrm::GetExtPmr());
    root->type = look.type;
    root->id = 1;
    root->position_grid = { 1, 2 };
    root->size_grid = { 2, 2 };
    if(look.icon != IconType::None)
        root->properties[WidgetPropertyType::ICON_TYPE] = static_cast<int>(look.icon);

    if(look.icon == IconType::Label)
        root->properties[WidgetPropertyType::LABEL] = std::pmr::string("Ab");

    std::vector<std::shared_ptr<WidgetConfiguration>> records{ root };
    if(look.type == WidgetType::IndicatorDial) {
        root->properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::pmr::vector<int>({ 2 }, Mrm::GetExtPmr());
        records.push_back(views_test::NeedleConfiguration(2, IconType::Rectangle));
    }

    return records;
}

std::shared_ptr<ScreenConfiguration> LookScreen(const std::vector<std::shared_ptr<WidgetConfiguration>>& records) {
    auto screen = MakeScreen();
    for(const auto& record : records)
        screen->AddWidget(record);

    return screen;
}

void ExpectSameLook(const IWidget& patched, const IWidget& fresh, const std::string& label, bool is_drawn = true) {
    lv_obj_t* const a = patched.GetContainer()->GetObject();
    lv_obj_t* const b = fresh.GetContainer()->GetObject();
    lv_obj_update_layout(a);
    lv_obj_update_layout(b);

    zassert_equal(lv_obj_get_x(a), lv_obj_get_x(b), "%s: x", label.c_str());
    zassert_equal(lv_obj_get_y(a), lv_obj_get_y(b), "%s: y", label.c_str());
    zassert_equal(lv_obj_get_width(a), lv_obj_get_width(b), "%s: width", label.c_str());
    zassert_equal(lv_obj_get_height(a), lv_obj_get_height(b), "%s: height", label.c_str());
    zassert_equal(lv_obj_is_hidden(a), lv_obj_is_hidden(b), "%s: hidden", label.c_str());
    zassert_equal(
        lv_obj_get_style_opa_layered(a, LV_PART_MAIN), lv_obj_get_style_opa_layered(b, LV_PART_MAIN), "%s: opacity",
        label.c_str()
    );

    // How far it is turned. (The pivot stays where the anchor put it while a widget does not rotate, as
    // widget_animations pins: it shows only when the rotation comes back, and then it is computed again.)
    zassert_equal(
        lv_obj_get_style_transform_rotation(WidgetContent(patched), LV_PART_MAIN),
        lv_obj_get_style_transform_rotation(WidgetContent(fresh), LV_PART_MAIN), "%s: rotation", label.c_str()
    );

    // And the pixels, of a widget that shows.
    // Nothing of a widget that is hidden, or has no size, shows to compare.
    if(!is_drawn || lv_obj_is_hidden(a) || lv_obj_get_width(a) == 0 || lv_obj_get_height(a) == 0)
        return;

    // One snapshot at a time, the first kept as plain pixels: LVGL's memory is not for two of them.
    std::vector<lv_color32_t> pixels_a;
    uint32_t width = 0;
    uint32_t height = 0;
    {
        std::unique_ptr<lv_draw_buf_t, decltype(&lv_draw_buf_destroy)> snapshot(
            lv_snapshot_take(a, LV_COLOR_FORMAT_ARGB8888), lv_draw_buf_destroy
        );
        zassert_not_null(snapshot.get(), "%s: snapshot of the patched widget", label.c_str());
        width = snapshot->header.w;
        height = snapshot->header.h;
        for(uint32_t row = 0; row < height; ++row) {
            const auto* pixels = views_test::PixelRow(*snapshot, row);
            pixels_a.insert(pixels_a.end(), pixels, pixels + width);
        }
    }

    std::unique_ptr<lv_draw_buf_t, decltype(&lv_draw_buf_destroy)> snapshot_b(
        lv_snapshot_take(b, LV_COLOR_FORMAT_ARGB8888), lv_draw_buf_destroy
    );
    zassert_not_null(snapshot_b.get(), "%s: snapshot of the built widget", label.c_str());
    zassert_equal(width, snapshot_b->header.w, "%s: snapshot width", label.c_str());
    zassert_equal(height, snapshot_b->header.h, "%s: snapshot height", label.c_str());
    size_t differing = 0;
    for(uint32_t row = 0; row < height; ++row) {
        const auto* pixels = views_test::PixelRow(*snapshot_b, row);
        for(uint32_t column = 0; column < width; ++column) {
            const auto& expected = pixels_a[row * width + column];
            if(pixels[column].red != expected.red
                || pixels[column].green != expected.green
                || pixels[column].blue != expected.blue
                || pixels[column].alpha != expected.alpha)
                ++differing;
        }
    }
    zassert_equal(differing, 0U, "%s: %u pixels differ", label.c_str(), static_cast<unsigned>(differing));
}

// One record changed to another: the screen of the old one patched, against the screen of the new one built.
void ExpectPatchedLooksBuilt(
    const std::vector<std::shared_ptr<WidgetConfiguration>>& old_records,
    const std::shared_ptr<WidgetConfiguration>& new_root,
    const std::string& label,
    bool is_drawn = true
) {

    auto new_records = old_records;
    new_records.front() = new_root;

    zassert_equal(
        ClassifyWidgetChange(*old_records.front(), *new_root), WidgetChange::InPlace, "%s: classified", label.c_str()
    );

    Scene patched(LookScreen(old_records));
    const auto plan = patched.Patch({ Put(new_root) });
    zassert_true(plan.effects[0] == PatchEffect::InPlace, "%s: planned", label.c_str());
    Scene fresh(LookScreen(new_records));

    ExpectSameLook(*Find(patched.screen, 1), *Find(fresh.screen, 1), label, is_drawn);
}

std::string LookLabel(const LookCase& look, WidgetPropertyType property, const char* direction) {
    return "widget " + std::to_string(static_cast<unsigned>(look.type)) + ", icon "
        + std::to_string(static_cast<unsigned>(look.icon)) + ", property "
        + std::to_string(static_cast<unsigned>(property)) + ", " + direction;
}

} // namespace

// The classifier says what is in place; this says it is true: for each type and each property it lists, the widget
// that took the change looks, to the pixel and to the position, as one built from the new record.
ZTEST(widget_patch, test_a_widget_patched_in_place_looks_as_one_built_from_the_new_record) {
    using P = WidgetPropertyType;
    size_t compared = 0;

    for(const auto& look : LOOK_CASES) {
        std::vector<std::pair<P, ConfigValue>> changes{
            { P::IS_VISIBLE, ConfigValue{ false } },
            { P::OPACITY, ConfigValue{ 100 } },
            { P::ANCHOR_POINT_X, ConfigValue{ 10 } },
            { P::ANCHOR_POINT_Y, ConfigValue{ 10 } },
            { P::COLOR_PRIMARY_ACTIVE, ConfigValue{ std::pmr::string("#E08020C0") } },
            { P::COLOR_PRIMARY_INACTIVE, ConfigValue{ std::pmr::string("#2080E0C0") } },
            { P::COLOR_SECONDARY_ACTIVE, ConfigValue{ std::pmr::string("#20E080C0") } },
            { P::COLOR_SECONDARY_INACTIVE, ConfigValue{ std::pmr::string("#E02080C0") } },
            { P::COLOR_TERTIARY_ACTIVE, ConfigValue{ std::pmr::string("#8020E0C0") } },
            { P::COLOR_TERTIARY_INACTIVE, ConfigValue{ std::pmr::string("#80E020C0") } },
        };
        if(look.type == WidgetType::BasicIcon && look.icon != IconType::Image && look.icon != IconType::Label) {
            changes.insert(
                changes.end(),
                { { P::POSITION_X, ConfigValue{ 6 } }, { P::POSITION_Y, ConfigValue{ 7 } },
                    { P::WIDTH_PX, ConfigValue{ 21 } }, { P::HEIGHT_PX, ConfigValue{ 17 } },
                    { P::STROKE_PX, ConfigValue{ 5 } }, { P::CORNER_RAD_PX, ConfigValue{ 4 } },
                    { P::FILL_MODE, ConfigValue{ 1 } }, { P::DIRECTION, ConfigValue{ 3 } } }
            );
        }

        for(const auto& [property, value] : changes) {
            // Not there to there.
            auto old_records = LookRecords(look);
            auto new_root = Copy(*old_records.front());
            new_root->properties[property] = value;
            ExpectPatchedLooksBuilt(old_records, new_root, LookLabel(look, property, "set"));

            // There to not there: back to the widget's own default.
            old_records.front()->properties[property] = value;
            ExpectPatchedLooksBuilt(
                old_records, Copy(*LookRecords(look).front()), LookLabel(look, property, "dropped")
            );
            compared += 2;
        }

        // The box.
        auto old_records = LookRecords(look);
        auto moved = Copy(*old_records.front());
        moved->position_grid = { 3, 1 };
        moved->size_grid = { 3, 2 };
        ExpectPatchedLooksBuilt(old_records, moved, LookLabel(look, P::LAYOUT_X, "moved"));
        ++compared;
    }

    zassert_true(compared > 300, "Compared %u", static_cast<unsigned>(compared));
}

// A rotation that is taken away leaves the widget as straight as one that never rotated.
ZTEST(widget_patch, test_a_widget_that_stops_rotating_is_not_left_turned) {
    using P = WidgetPropertyType;
    for(const auto& look : { LookCase{ WidgetType::BasicIcon, IconType::Rectangle },
            LookCase{ WidgetType::IndicatorBar, IconType::None } }) {
        auto old_records = LookRecords(look);
        old_records.front()->properties[P::ANCHOR_POINT_X] = 5;
        old_records.front()->properties[P::ANCHOR_POINT_Y] = 7;
        old_records.front()->properties[P::ANIMATION_TYPE] = 2;
        old_records.front()->properties[P::ANIMATION_DURATION_MS] = 1000;
        old_records.front()->properties[P::IS_ANIMATION_ACTIVE] = true;

        auto new_root = Copy(*old_records.front());
        new_root->properties[P::ANIMATION_TYPE] = 0;
        new_root->properties.erase(P::ANIMATION_DURATION_MS);
        new_root->properties.erase(P::IS_ANIMATION_ACTIVE);
        // (A turned widget is too large for the snapshot; how far it is turned is compared.)
        ExpectPatchedLooksBuilt(old_records, new_root, LookLabel(look, P::ANIMATION_TYPE, "rotation stopped"), false);
    }
}

ZTEST(widget_patch, test_a_property_an_event_drives_keeps_the_value_the_event_gave_through_a_patch) {
    constexpr const char* OPACITY_SENSOR = "patch_opacity";
    constexpr const char* X_SENSOR = "patch_x";

    const auto bind = [](WidgetPropertyType target, const char* sensor_id) {
        return PropertyBinding{ .target = target,
            .event = { .channel = EventChannelId::Sensors,
                .event_type = std::to_underlying(SensorEventType::DataUpdated),
                .payload_key = std::to_underlying(SensorPayloadType::Value),
                .selector_key = std::to_underlying(SensorPayloadType::SensorId),
                .selector_value = std::pmr::string(sensor_id, Mrm::GetExtPmr()) } };
    };
    const auto publish = [](const char* sensor_id, int value) {
        SensorEventsChannel::GetInstance().Publish(
            { .source_id = 0,
                .type = SensorEventType::DataUpdated,
                .payload = { { SensorPayloadType::SensorId, StringHelpers::GetHash(sensor_id) },
                    { SensorPayloadType::Value, value } } }
        );
    };

    auto bar = Bar(1, 0, 2);
    bar->properties[WidgetPropertyType::OPACITY] = 255;
    bar->bindings.push_back(bind(WidgetPropertyType::OPACITY, OPACITY_SENSOR));
    bar->bindings.push_back(bind(WidgetPropertyType::LAYOUT_X, X_SENSOR));
    auto screen_configuration = MakeScreen();
    screen_configuration->AddWidget(bar);
    Scene scene(screen_configuration);
    auto* widget = Find(scene.screen, 1);
    lv_obj_t* const container = widget->GetContainer()->GetObject();

    publish(OPACITY_SENSOR, 128);
    publish(X_SENSOR, 5);
    const auto layout = GridLayout::FromActiveScreen(scene.configuration->grid);
    zassert_equal(lv_obj_get_style_opa_layered(container, LV_PART_MAIN), 128);
    zassert_equal(widget->GetPositionPx().x, layout.ToPx(WidgetPosition{ 5, 0 }, widget->GetSizePx()).x);

    // The record changes what the events drive, and the box it is the default of.
    auto edited = Copy(*bar);
    edited->bindings.assign(bar->bindings.begin(), bar->bindings.end());
    edited->properties[WidgetPropertyType::OPACITY] = 60;
    edited->position_grid = { 3, 4 };
    edited->size_grid = { 3, 2 };
    scene.Patch({ Put(edited) });

    zassert_equal(Find(scene.screen, 1), widget);
    zassert_equal(lv_obj_get_style_opa_layered(container, LV_PART_MAIN), 128, "The bound opacity is the event's");
    zassert_equal(
        widget->GetPositionPx().x, layout.ToPx(WidgetPosition{ 5, 4 }, widget->GetSizePx()).x, "The bound x too"
    );
    zassert_equal(
        widget->GetSizePx().width, layout.ToPx(edited->size_grid).width, "and the unbound size is the record's"
    );

    // The next event is the live value again.
    publish(OPACITY_SENSOR, 200);
    zassert_equal(lv_obj_get_style_opa_layered(container, LV_PART_MAIN), 200);
}

ZTEST(widget_patch, test_a_value_the_widget_refuses_is_left_out_as_a_built_widget_leaves_it_out) {
    auto bar = Bar(1, 0, 0);
    bar->properties[WidgetPropertyType::COLOR_PRIMARY_ACTIVE] = std::pmr::string("#33CC99FF");
    auto screen_configuration = MakeScreen();
    screen_configuration->AddWidget(bar);
    Scene scene(screen_configuration);
    auto* widget = Find(scene.screen, 1);

    // A color that is no color: the validator keeps it out of a patch, but the widget does not depend on that.
    auto refused = Copy(*bar);
    refused->properties[WidgetPropertyType::COLOR_PRIMARY_ACTIVE] = std::pmr::string("not a color");
    widget->ApplyEditorConfiguration(refused);

    CheckBackground(
        Inner(*widget), LV_PART_INDICATOR, ThemeManager::GetInstance().GetCurrentTheme().GetSecondaryColor()
    );
}
