#include <array>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <eerie_memory.hpp>
#include <zephyr/ztest.h>

#include "utilities/memory/memory_resource_manager.h"

#include "domain/ui_domain/configuration/parsers/ui_configuration_cbor_parser.h"
#include "domain/ui_domain/models/icon_type.h"
#include "domain/ui_domain/models/property_binding.h"
#include "domain/ui_domain/models/ui_configuration.h"
#include "domain/ui_domain/models/widget_property.h"
#include "domain/ui_domain/models/widget_type.h"
#include "domain/ui_domain/preview/ui_patch.h"

#include "ui_patch_records.h"

using namespace eerie_memory;
using namespace eerie_leap::utilities::memory;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::domain::ui_domain::preview;

using eerie_leap::domain::ui_domain::configuration::parsers::UiConfigurationCborParser;
using eerie_leap::subsys::event_bus::EventChannelId;
using eerie_leap::utilities::type::ConfigValue;

namespace {

std::pmr::memory_resource* Mr() {
    return Mrm::GetDefaultPmr();
}

ConfigValue Text(const char* text) {
    return ConfigValue{ std::pmr::string(text, Mr()) };
}

ConfigValue Ints(std::initializer_list<int> values) {
    return ConfigValue{ std::pmr::vector<int>(values, Mr()) };
}

std::shared_ptr<WidgetConfiguration> MakeWidget(WidgetType type, uint32_t id, int32_t z_index = 0) {
    auto widget = make_shared_pmr<WidgetConfiguration>(Mr());
    widget->type = type;
    widget->id = id;
    widget->position_grid = { 1, 2 };
    widget->size_grid = { 3, 3 };
    widget->z_index = z_index;

    return widget;
}

std::shared_ptr<WidgetConfiguration> Copy(const WidgetConfiguration& source) {
    auto widget = make_shared_pmr<WidgetConfiguration>(Mr());
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

std::shared_ptr<ScreenConfiguration> MakeScreen(uint32_t id, uint32_t group_id) {
    auto screen = make_shared_pmr<ScreenConfiguration>(Mr());
    screen->id = id;
    screen->screen_group_id = group_id;
    screen->type = ScreenType::Gauge;
    screen->grid = { .snap_enabled = true, .width = 10, .height = 10, .spacing_px = 0 };

    return screen;
}

std::shared_ptr<UiConfiguration> MakeConfiguration(std::initializer_list<std::shared_ptr<ScreenConfiguration>> screens) {
    auto configuration = make_shared_pmr<UiConfiguration>(Mr());
    configuration->active_screen_group_id = 1;
    for(const auto& screen : screens)
        configuration->screen_configurations.push_back(screen);

    return configuration;
}

// The class of a change to one property of one widget; a monostate is a property the record lacks.
WidgetChange ClassifyProperty(
    WidgetType type,
    WidgetPropertyType key,
    ConfigValue before,
    ConfigValue after,
    std::optional<IconType> icon = std::nullopt
) {
    auto old_widget = MakeWidget(type, 4);
    auto new_widget = MakeWidget(type, 4);
    if(icon.has_value()) {
        old_widget->properties.emplace(WidgetPropertyType::ICON_TYPE, static_cast<int>(*icon));
        new_widget->properties.emplace(WidgetPropertyType::ICON_TYPE, static_cast<int>(*icon));
    }

    if(!std::holds_alternative<std::monostate>(before))
        old_widget->properties[key] = std::move(before);

    if(!std::holds_alternative<std::monostate>(after))
        new_widget->properties[key] = std::move(after);

    return ClassifyWidgetChange(*old_widget, *new_widget);
}

} // namespace

ZTEST_SUITE(ui_patch, NULL, NULL, NULL, NULL, NULL);

// The table of plan `gauge-ui-editor.md`, Appendix B ("What applies in place" and "What needs the widget rebuilt").
ZTEST(ui_patch, test_the_classifier_applies_in_place_what_a_widget_takes_as_it_changes) {
    using P = WidgetPropertyType;
    using T = WidgetType;
    const ConfigValue none;

    // Every widget: management, colors, anchors and the animation.
    for(const auto type : { T::IndicatorBar, T::IndicatorDigital, T::BasicLottie, T::ControlButton, T::BasicIcon }) {
        zassert_equal(ClassifyProperty(type, P::IS_VISIBLE, true, false), WidgetChange::InPlace);
        zassert_equal(ClassifyProperty(type, P::IS_ACTIVE, none, false), WidgetChange::InPlace);
        zassert_equal(ClassifyProperty(type, P::OPACITY, 255, 128), WidgetChange::InPlace);
        zassert_equal(ClassifyProperty(type, P::COLOR_PRIMARY_ACTIVE, Text("#FF0000FF"), Text("#00FF00FF")), WidgetChange::InPlace);
        zassert_equal(ClassifyProperty(type, P::COLOR_TERTIARY_INACTIVE, Text("#FF0000FF"), none), WidgetChange::InPlace);
        zassert_equal(ClassifyProperty(type, P::ANCHOR_POINT_X, -1, 10), WidgetChange::InPlace);
        zassert_equal(ClassifyProperty(type, P::ANIMATION_TYPE, 0, 1), WidgetChange::InPlace);
        zassert_equal(ClassifyProperty(type, P::IS_ANIMATION_ACTIVE, none, true), WidgetChange::InPlace);
        zassert_equal(ClassifyProperty(type, P::ANIMATION_DURATION_MS, 500, 800), WidgetChange::InPlace);
    }

    // Indicators: range, value, precision, unit, label, angles.
    zassert_equal(ClassifyProperty(T::IndicatorBar, P::MAX_VALUE, 100.0, 200.0), WidgetChange::InPlace);
    zassert_equal(ClassifyProperty(T::IndicatorBar, P::MIN_VALUE, none, 5.0), WidgetChange::InPlace);
    zassert_equal(ClassifyProperty(T::IndicatorBar, P::VALUE, 1.0, none), WidgetChange::InPlace);
    zassert_equal(ClassifyProperty(T::IndicatorDigital, P::VALUE_PRECISION, 1, 2), WidgetChange::InPlace);
    zassert_equal(ClassifyProperty(T::IndicatorBar, P::VALUE_PRECISION, 1, 2), WidgetChange::RebuildWidget, "A bar has none");
    zassert_equal(ClassifyProperty(T::IndicatorSetting, P::UNIT, Text("a"), Text("b")), WidgetChange::InPlace);
    zassert_equal(ClassifyProperty(T::IndicatorSetting, P::LABEL, none, Text("b")), WidgetChange::InPlace);
    zassert_equal(ClassifyProperty(T::IndicatorSetting, P::VALUE_PRECISION, 0, 1), WidgetChange::InPlace);
    zassert_equal(ClassifyProperty(T::IndicatorSetting, P::SETTING_ID, Text("a"), Text("b")), WidgetChange::RebuildWidget);
    zassert_equal(ClassifyProperty(T::IndicatorDigital, P::UNIT, Text("a"), Text("b")), WidgetChange::RebuildWidget);
    for(const auto type : { T::IndicatorDial, T::IndicatorSegmentArc, T::IndicatorArcFill }) {
        zassert_equal(ClassifyProperty(type, P::START_ANGLE, 45, 90), WidgetChange::InPlace);
        zassert_equal(ClassifyProperty(type, P::END_ANGLE, none, 300), WidgetChange::InPlace);
        zassert_equal(ClassifyProperty(type, P::MAX_VALUE, 1.0, 2.0), WidgetChange::InPlace);
    }

    // Controls.
    zassert_equal(ClassifyProperty(T::ControlButton, P::LABEL, Text("a"), Text("b")), WidgetChange::InPlace);
    zassert_equal(ClassifyProperty(T::ControlButton, P::NAVIGATION_INTENT, 1, 2), WidgetChange::RebuildWidget);
    zassert_equal(ClassifyProperty(T::ControlButton, P::TARGET_SCREEN_GROUP, 1, 2), WidgetChange::RebuildWidget);
    zassert_equal(ClassifyProperty(T::ControlToggle, P::VALUE, false, true), WidgetChange::InPlace);
    zassert_equal(ClassifyProperty(T::ControlSlider, P::MIN_VALUE, 0.0, 1.0), WidgetChange::InPlace);
    zassert_equal(ClassifyProperty(T::ControlSlider, P::STEP, 1.0, 2.0), WidgetChange::RebuildWidget);

    // The chart's points and type, a file and the animation's size rebuild the widget.
    zassert_equal(ClassifyProperty(T::IndicatorHorizontalChart, P::CHART_POINT_COUNT, 30, 40), WidgetChange::RebuildWidget);
    zassert_equal(ClassifyProperty(T::IndicatorHorizontalChart, P::CHART_TYPE, 0, 1), WidgetChange::RebuildWidget);
    zassert_equal(ClassifyProperty(T::IndicatorHorizontalChart, P::MAX_VALUE, 1.0, 2.0), WidgetChange::InPlace);
    zassert_equal(ClassifyProperty(T::BasicLottie, P::FILE_PATH, Text("a.json"), Text("b.json")), WidgetChange::RebuildWidget);
    zassert_equal(ClassifyProperty(T::BasicLottie, P::WIDTH_PX, 10, 20), WidgetChange::RebuildWidget);

    // What the classifier does not know is a rebuild, which is always right.
    zassert_equal(ClassifyProperty(T::IndicatorBar, P::IS_SMOOTHED, false, true), WidgetChange::RebuildWidget);
    zassert_equal(ClassifyProperty(T::IndicatorDial, P::CHILD_WIDGET_IDS, Ints({ 1 }), Ints({ 1, 2 })), WidgetChange::RebuildWidget);
    zassert_equal(ClassifyProperty(T::IndicatorBar, P::FILE_PATH, none, Text("x")), WidgetChange::RebuildWidget);
    zassert_equal(ClassifyProperty(T::IndicatorBar, P::FILE_PATH, Text("x"), none), WidgetChange::RebuildWidget);
}

ZTEST(ui_patch, test_the_classifier_reads_the_icon_type_for_an_icon) {
    using P = WidgetPropertyType;
    using T = WidgetType;
    const ConfigValue none;

    for(const auto icon : { IconType::Rectangle, IconType::Oval, IconType::TriangleIsosceles, IconType::TriangleRight, IconType::Line }) {
        for(const auto type : { T::BasicIcon, T::BasicArcIcon }) {
            zassert_equal(ClassifyProperty(type, P::WIDTH_PX, 10, 20, icon), WidgetChange::InPlace);
            zassert_equal(ClassifyProperty(type, P::HEIGHT_PX, none, 20, icon), WidgetChange::InPlace);
            zassert_equal(ClassifyProperty(type, P::STROKE_PX, 2, 4, icon), WidgetChange::InPlace);
            zassert_equal(ClassifyProperty(type, P::CORNER_RAD_PX, 2, 4, icon), WidgetChange::InPlace);
            zassert_equal(ClassifyProperty(type, P::FILL_MODE, 0, 1, icon), WidgetChange::InPlace);
            zassert_equal(ClassifyProperty(type, P::DIRECTION, 1, 2, icon), WidgetChange::InPlace);
            zassert_equal(ClassifyProperty(type, P::POSITION_X, 0, 5, icon), WidgetChange::InPlace);
            zassert_equal(ClassifyProperty(type, P::POSITION_Y, 0, 5, icon), WidgetChange::InPlace);
        }
    }

    for(const auto icon : { IconType::Image, IconType::Label }) {
        zassert_equal(ClassifyProperty(T::BasicIcon, P::POSITION_X, 0, 5, icon), WidgetChange::InPlace);
        zassert_equal(ClassifyProperty(T::BasicIcon, P::COLOR_PRIMARY_ACTIVE, none, Text("#FF0000FF"), icon), WidgetChange::InPlace);
        zassert_equal(ClassifyProperty(T::BasicIcon, P::WIDTH_PX, 10, 20, icon), WidgetChange::RebuildWidget);
        zassert_equal(ClassifyProperty(T::BasicIcon, P::FILL_MODE, 0, 1, icon), WidgetChange::RebuildWidget);
    }

    zassert_equal(ClassifyProperty(T::BasicIcon, P::FILE_PATH, Text("a.bin"), Text("b.bin"), IconType::Image), WidgetChange::RebuildWidget);
    zassert_equal(ClassifyProperty(T::BasicIcon, P::IMG_WIDTH, 10, 20, IconType::Image), WidgetChange::RebuildWidget);
    zassert_equal(ClassifyProperty(T::BasicIcon, P::LABEL, Text("a"), Text("b"), IconType::Label), WidgetChange::RebuildWidget, "A label icon's label");
    zassert_equal(ClassifyProperty(T::BasicArcIcon, P::POSITION_ANGLE, -90.0, 0.0, IconType::Rectangle), WidgetChange::RebuildWidget);
    zassert_equal(ClassifyProperty(T::BasicArcIcon, P::EDGE_OFFSET, 0, 4, IconType::Rectangle), WidgetChange::RebuildWidget);

    // The icon type itself.
    auto before = MakeWidget(T::BasicIcon, 1);
    before->properties.emplace(P::ICON_TYPE, static_cast<int>(IconType::Rectangle));
    auto after = Copy(*before);
    after->properties[P::ICON_TYPE] = static_cast<int>(IconType::Oval);
    zassert_equal(ClassifyWidgetChange(*before, *after), WidgetChange::RebuildWidget);
}

ZTEST(ui_patch, test_the_classifier_takes_the_box_and_the_order_in_place_and_the_type_and_bindings_not) {
    auto before = MakeWidget(WidgetType::IndicatorBar, 3);
    zassert_equal(ClassifyWidgetChange(*before, *Copy(*before)), WidgetChange::InPlace);

    auto moved = Copy(*before);
    moved->position_grid = { 4, 5 };
    moved->size_grid = { 6, 7 };
    moved->z_index = 9;
    zassert_equal(ClassifyWidgetChange(*before, *moved), WidgetChange::InPlace);

    auto retyped = Copy(*before);
    retyped->type = WidgetType::IndicatorDigital;
    zassert_equal(ClassifyWidgetChange(*before, *retyped), WidgetChange::RebuildWidget);

    PropertyBinding binding;
    binding.target = WidgetPropertyType::VALUE;
    binding.event.channel = EventChannelId::Sensors;
    binding.event.event_type = 1;
    binding.event.payload_key = 2;

    auto bound = Copy(*before);
    bound->bindings.push_back(binding);
    zassert_equal(ClassifyWidgetChange(*before, *bound), WidgetChange::RebuildWidget, "A binding was added");
    zassert_equal(ClassifyWidgetChange(*bound, *Copy(*bound)), WidgetChange::InPlace, "The same bindings");

    auto rebound = Copy(*bound);
    rebound->bindings.front().event.payload_key = 3;
    zassert_equal(ClassifyWidgetChange(*bound, *rebound), WidgetChange::RebuildWidget, "A binding changed");

    auto redirected = Copy(*bound);
    redirected->bindings.front().direction = PropertyBindingDirection::Out;
    zassert_equal(ClassifyWidgetChange(*bound, *redirected), WidgetChange::RebuildWidget);

    zassert_true(IsSameWidget(*bound, *Copy(*bound)));
    zassert_false(IsSameWidget(*bound, *rebound));
    zassert_false(IsSameWidget(*before, *moved));
}

ZTEST(ui_patch, test_a_screen_changes_in_place_but_for_its_own_record) {
    auto before = MakeScreen(1, 5);
    zassert_equal(ClassifyScreenChange(*before, *MakeScreen(1, 5)), WidgetChange::InPlace);

    auto widgets = MakeScreen(1, 5);
    widgets->AddWidget(MakeWidget(WidgetType::IndicatorBar, 1));
    zassert_equal(ClassifyScreenChange(*before, *widgets), WidgetChange::InPlace, "Widgets are not the screen's record");

    for(int field = 0; field < 8; ++field) {
        auto changed = MakeScreen(1, 5);
        switch(field) {
            case 0: changed->screen_group_id = 6; break;
            case 1: changed->type = ScreenType::Settings; break;
            case 2: changed->z_index = 3; break;
            case 3: changed->is_visible = false; break;
            case 4: changed->is_overlay = true; break;
            case 5: changed->grid.width = 11; break;
            case 6: changed->grid.snap_enabled = false; break;
            default: changed->grid.spacing_px = 2; break;
        }

        zassert_equal(ClassifyScreenChange(*before, *changed), WidgetChange::RebuildGroup, "Field %d", field);
    }
}

ZTEST(ui_patch, test_patch_operations_apply_in_order_onto_a_copy_and_share_what_they_leave) {
    auto screen_a = MakeScreen(1, 5);
    screen_a->AddWidget(MakeWidget(WidgetType::IndicatorBar, 10, 0));
    screen_a->AddWidget(MakeWidget(WidgetType::IndicatorBar, 11, 1));
    screen_a->AddWidget(MakeWidget(WidgetType::IndicatorBar, 12, 2));
    auto screen_b = MakeScreen(2, 5);
    screen_b->AddWidget(MakeWidget(WidgetType::IndicatorBar, 20));
    const auto base = MakeConfiguration({ screen_a, screen_b });

    auto replaced = Copy(*screen_a->widget_configurations[1]);
    replaced->properties[WidgetPropertyType::MAX_VALUE] = 5.0;
    PatchOp replace{ .kind = PatchOpKind::PutWidget, .screen_id = 1, .widget = replaced };

    // Another z-index: it takes its place among the widgets of that z-index.
    auto lifted = Copy(*screen_a->widget_configurations[0]);
    lifted->z_index = 5;
    PatchOp lift{ .kind = PatchOpKind::PutWidget, .screen_id = 1, .widget = lifted };

    PatchOp add{ .kind = PatchOpKind::PutWidget, .screen_id = 1, .widget = MakeWidget(WidgetType::IndicatorBar, 13, 1) };
    const std::array ops{ replace, lift, add };

    const auto patched = ApplyPatchOps(*base, ops, Mr());

    zassert_not_equal(patched, base);
    zassert_equal(patched->screen_configurations.size(), 2U);
    zassert_equal(patched->screen_configurations[1], screen_b, "A screen no operation touched is shared");
    zassert_not_equal(patched->screen_configurations[0], screen_a, "A touched one is a copy");
    zassert_equal(screen_a->widget_configurations.size(), 3U, "and the base is as it was");
    zassert_equal(screen_a->widget_configurations[1]->properties.size(), 0U);

    const auto& widgets = patched->screen_configurations[0]->widget_configurations;
    zassert_equal(widgets.size(), 4U);
    zassert_equal(widgets[0]->id, 11U);
    zassert_equal(widgets[0], replaced);
    zassert_equal(widgets[1]->id, 13U, "Added after the widgets of its z-index");
    zassert_equal(widgets[2]->id, 12U);
    zassert_equal(widgets[3]->id, 10U, "Moved to its new z-index");
    zassert_equal(widgets[2], screen_a->widget_configurations[2], "A widget no operation touched is shared");
}

ZTEST(ui_patch, test_a_replaced_widget_keeps_its_place_among_equal_z_indices) {
    auto screen = MakeScreen(1, 5);
    screen->AddWidget(MakeWidget(WidgetType::IndicatorBar, 10));
    screen->AddWidget(MakeWidget(WidgetType::IndicatorBar, 11));
    screen->AddWidget(MakeWidget(WidgetType::IndicatorBar, 12));
    const auto base = MakeConfiguration({ screen });

    auto replaced = Copy(*screen->widget_configurations[0]);
    replaced->properties[WidgetPropertyType::MAX_VALUE] = 5.0;
    const std::array ops{ PatchOp{ .kind = PatchOpKind::PutWidget, .screen_id = 1, .widget = replaced } };

    const auto patched = ApplyPatchOps(*base, ops, Mr());
    const auto& widgets = patched->screen_configurations[0]->widget_configurations;

    zassert_equal(widgets[0], replaced);
    zassert_equal(widgets[1]->id, 11U);
    zassert_equal(widgets[2]->id, 12U);
}

ZTEST(ui_patch, test_removing_a_widget_removes_what_it_owns) {
    auto screen = MakeScreen(1, 5);
    auto dial = MakeWidget(WidgetType::IndicatorDial, 10);
    dial->properties.emplace(WidgetPropertyType::CHILD_WIDGET_IDS, Ints({ 11, 12 }));
    auto needle = MakeWidget(WidgetType::BasicIcon, 11);
    needle->properties.emplace(WidgetPropertyType::CHILD_WIDGET_IDS, Ints({ 13 }));
    screen->AddWidget(dial);
    screen->AddWidget(needle);
    screen->AddWidget(MakeWidget(WidgetType::BasicIcon, 12));
    screen->AddWidget(MakeWidget(WidgetType::BasicIcon, 13));
    screen->AddWidget(MakeWidget(WidgetType::IndicatorBar, 14));
    const auto base = MakeConfiguration({ screen });

    const std::array ops{ PatchOp{ .kind = PatchOpKind::RemoveWidget, .screen_id = 1, .widget_id = 10 } };
    const auto patched = ApplyPatchOps(*base, ops, Mr());

    const auto& widgets = patched->screen_configurations[0]->widget_configurations;
    zassert_equal(widgets.size(), 1U);
    zassert_equal(widgets[0]->id, 14U);
}

ZTEST(ui_patch, test_screens_are_put_and_removed) {
    auto screen_a = MakeScreen(1, 5);
    auto screen_b = MakeScreen(2, 5);
    const auto base = MakeConfiguration({ screen_a, screen_b });

    auto replacement = MakeScreen(1, 6);
    auto added = MakeScreen(3, 6);
    added->AddWidget(MakeWidget(WidgetType::IndicatorBar, 1));
    const std::array ops{ PatchOp{ .kind = PatchOpKind::PutScreen, .screen = replacement },
        PatchOp{ .kind = PatchOpKind::PutScreen, .screen = added },
        PatchOp{ .kind = PatchOpKind::RemoveScreen, .screen_id = 2 } };

    const auto patched = ApplyPatchOps(*base, ops, Mr());

    zassert_equal(patched->screen_configurations.size(), 2U);
    zassert_equal(patched->screen_configurations[0], replacement, "In the place of the screen of its ID");
    zassert_equal(patched->screen_configurations[1], added);
    zassert_equal(base->screen_configurations.size(), 2U);
    zassert_equal(base->screen_configurations[0], screen_a);
}

ZTEST(ui_patch, test_an_operation_on_what_the_base_lacks_is_a_mismatch_and_changes_nothing) {
    auto screen = MakeScreen(1, 5);
    screen->AddWidget(MakeWidget(WidgetType::IndicatorBar, 10));
    const auto base = MakeConfiguration({ screen });

    const auto mismatches = [&base](PatchOp op) {
        // Behind a good operation, which an all-or-nothing patch does not keep.
        const std::array ops{ PatchOp{ .kind = PatchOpKind::PutWidget, .screen_id = 1, .widget = MakeWidget(WidgetType::IndicatorBar, 11) },
            std::move(op) };
        try {
            (void)ApplyPatchOps(*base, ops, Mr());
        } catch(const PatchBaseMismatch&) {
            return true;
        }

        return false;
    };

    zassert_true(mismatches({ .kind = PatchOpKind::PutWidget, .screen_id = 9, .widget = MakeWidget(WidgetType::IndicatorBar, 1) }));
    zassert_true(mismatches({ .kind = PatchOpKind::RemoveWidget, .screen_id = 9, .widget_id = 10 }));
    zassert_true(mismatches({ .kind = PatchOpKind::RemoveWidget, .screen_id = 1, .widget_id = 99 }));
    zassert_true(mismatches({ .kind = PatchOpKind::RemoveScreen, .screen_id = 9 }));
    zassert_equal(screen->widget_configurations.size(), 1U);
    zassert_equal(base->screen_configurations[0], screen);

    // An operation after one that removed what it names.
    const std::array ops{ PatchOp{ .kind = PatchOpKind::RemoveWidget, .screen_id = 1, .widget_id = 10 },
        PatchOp{ .kind = PatchOpKind::RemoveWidget, .screen_id = 1, .widget_id = 10 } };
    bool is_mismatch = false;
    try {
        (void)ApplyPatchOps(*base, ops, Mr());
    } catch(const PatchBaseMismatch&) {
        is_mismatch = true;
    }
    zassert_true(is_mismatch);
}

namespace {

struct PlanFixture {
    std::shared_ptr<ScreenConfiguration> screen = MakeScreen(1, 5);
    std::shared_ptr<UiConfiguration> base;

    PlanFixture() {
        auto bar = MakeWidget(WidgetType::IndicatorBar, 10, 0);
        bar->properties.emplace(WidgetPropertyType::MAX_VALUE, 100.0);
        screen->AddWidget(bar);
        auto dial = MakeWidget(WidgetType::IndicatorDial, 20, 1);
        dial->properties.emplace(WidgetPropertyType::CHILD_WIDGET_IDS, Ints({ 21 }));
        screen->AddWidget(dial);
        auto needle = MakeWidget(WidgetType::BasicIcon, 21, 2);
        needle->properties.emplace(WidgetPropertyType::ICON_TYPE, static_cast<int>(IconType::Rectangle));
        screen->AddWidget(needle);
        base = MakeConfiguration({ screen, MakeScreen(2, 5) });
        base->screen_configurations[1]->AddWidget(MakeWidget(WidgetType::IndicatorBar, 30));
    }

    PatchPlan Plan(std::span<const PatchOp> ops) const {
        const auto patched = ApplyPatchOps(*base, ops, Mr());

        return PlanPatch(*base, *patched, ops);
    }
};

} // namespace

ZTEST(ui_patch, test_a_color_is_updated_in_place_and_a_file_rebuilds_the_widget) {
    PlanFixture fixture;

    auto recolored = Copy(*fixture.screen->widget_configurations[0]);
    recolored->properties[WidgetPropertyType::COLOR_PRIMARY_ACTIVE] = Text("#00FF00FF");
    recolored->position_grid.x = 7;
    const std::array in_place{ PatchOp{ .kind = PatchOpKind::PutWidget, .screen_id = 1, .widget = recolored } };

    auto plan = fixture.Plan(in_place);
    zassert_equal(plan.screens.size(), 1U);
    zassert_true(plan.screens[0].action == ScreenAction::Widgets);
    zassert_equal(plan.screens[0].widgets.size(), 1U);
    zassert_equal(plan.screens[0].widgets[0].widget_id, 10U);
    zassert_true(plan.screens[0].widgets[0].action == WidgetAction::Update);
    zassert_equal(plan.effects.size(), 1U);
    zassert_true(plan.effects[0] == PatchEffect::InPlace);
    zassert_equal(plan.op_screen_ids[0], 1U);

    auto retyped = Copy(*recolored);
    retyped->properties[WidgetPropertyType::FILE_PATH] = Text("x.bin");
    const std::array rebuilds{ PatchOp{ .kind = PatchOpKind::PutWidget, .screen_id = 1, .widget = retyped } };

    plan = fixture.Plan(rebuilds);
    zassert_true(plan.screens[0].widgets[0].action == WidgetAction::Replace);
    zassert_true(plan.effects[0] == PatchEffect::WidgetRebuilt);
}

ZTEST(ui_patch, test_what_a_child_changes_rebuilds_its_owner_and_a_patch_that_changes_nothing_plans_nothing) {
    PlanFixture fixture;

    // The needle's own color is in place for a root, but it is the dial's child.
    auto recolored = Copy(*fixture.screen->widget_configurations[2]);
    recolored->properties[WidgetPropertyType::COLOR_PRIMARY_ACTIVE] = Text("#00FF00FF");
    const std::array ops{ PatchOp{ .kind = PatchOpKind::PutWidget, .screen_id = 1, .widget = recolored } };

    const auto plan = fixture.Plan(ops);
    zassert_equal(plan.screens.size(), 1U);
    zassert_equal(plan.screens[0].widgets.size(), 1U);
    zassert_equal(plan.screens[0].widgets[0].widget_id, 20U, "The owner");
    zassert_true(plan.screens[0].widgets[0].action == WidgetAction::Replace);
    zassert_true(plan.effects[0] == PatchEffect::WidgetRebuilt);

    // The same record again.
    const std::array same{ PatchOp{ .kind = PatchOpKind::PutWidget, .screen_id = 1, .widget = Copy(*fixture.screen->widget_configurations[0]) } };
    const auto nothing = fixture.Plan(same);
    zassert_true(nothing.screens.empty());
    zassert_true(nothing.effects[0] == PatchEffect::InPlace);
}

ZTEST(ui_patch, test_widgets_are_added_and_removed_and_each_operation_has_its_effect) {
    PlanFixture fixture;

    const std::array ops{
        PatchOp{ .kind = PatchOpKind::PutWidget, .screen_id = 1, .widget = MakeWidget(WidgetType::IndicatorBar, 11, 3) },
        PatchOp{ .kind = PatchOpKind::RemoveWidget, .screen_id = 1, .widget_id = 10 },
        PatchOp{ .kind = PatchOpKind::RemoveWidget, .screen_id = 1, .widget_id = 20 },
    };
    const auto plan = fixture.Plan(ops);

    zassert_equal(plan.screens.size(), 1U);
    const auto& widgets = plan.screens[0].widgets;
    zassert_equal(widgets.size(), 3U);
    // Removed first.
    zassert_true(widgets[0].action == WidgetAction::Remove);
    zassert_true(widgets[1].action == WidgetAction::Remove);
    zassert_true(widgets[2].action == WidgetAction::Add);
    zassert_equal(widgets[2].widget_id, 11U);

    zassert_equal(plan.effects.size(), 3U);
    for(const auto effect : plan.effects)
        zassert_true(effect == PatchEffect::WidgetRebuilt);
}

ZTEST(ui_patch, test_a_change_of_the_screens_own_record_rebuilds_the_group) {
    PlanFixture fixture;

    auto regridded = MakeScreen(1, 5);
    regridded->grid.width = 20;
    for(const auto& widget : fixture.screen->widget_configurations)
        regridded->AddWidget(widget);

    auto added = MakeScreen(3, 5);
    const std::array ops{ PatchOp{ .kind = PatchOpKind::PutScreen, .screen = regridded },
        PatchOp{ .kind = PatchOpKind::PutScreen, .screen = added },
        PatchOp{ .kind = PatchOpKind::RemoveScreen, .screen_id = 2 } };
    const auto plan = fixture.Plan(ops);

    zassert_equal(plan.screens.size(), 3U);
    zassert_true(plan.screens[0].action == ScreenAction::Remove, "Removals first");
    zassert_equal(plan.screens[0].screen_id, 2U);
    zassert_true(plan.screens[1].action == ScreenAction::Rebuild);
    zassert_true(plan.screens[2].action == ScreenAction::Add);

    for(const auto effect : plan.effects)
        zassert_true(effect == PatchEffect::GroupRebuilt);
}

ZTEST(ui_patch, test_a_put_screen_with_only_a_recolored_widget_is_in_place) {
    PlanFixture fixture;

    // What an editor that always sends the whole screen would send: nothing but the widget differs.
    auto screen = MakeScreen(1, 5);
    for(const auto& widget : fixture.screen->widget_configurations) {
        auto copy = Copy(*widget);
        if(copy->id == 10)
            copy->properties[WidgetPropertyType::COLOR_PRIMARY_ACTIVE] = Text("#00FF00FF");

        screen->AddWidget(copy);
    }

    const std::array ops{ PatchOp{ .kind = PatchOpKind::PutScreen, .screen = screen } };
    const auto plan = fixture.Plan(ops);

    zassert_equal(plan.screens.size(), 1U);
    zassert_true(plan.screens[0].action == ScreenAction::Widgets);
    zassert_equal(plan.screens[0].widgets.size(), 1U);
    zassert_true(plan.screens[0].widgets[0].action == WidgetAction::Update);
    zassert_true(plan.effects[0] == PatchEffect::InPlace);
}

// ======== The records a patch carries ========

ZTEST(ui_patch, test_a_widget_record_decodes_to_the_widget_it_was_written_from) {
    auto widget = MakeWidget(WidgetType::IndicatorDial, 7, -2);
    widget->properties.emplace(WidgetPropertyType::MAX_VALUE, 250.5);
    widget->properties.emplace(WidgetPropertyType::VALUE_PRECISION, 2);
    widget->properties.emplace(WidgetPropertyType::UNIT, Text("km/h"));
    widget->properties.emplace(WidgetPropertyType::IS_VISIBLE, false);
    widget->properties.emplace(WidgetPropertyType::CHILD_WIDGET_IDS, Ints({ 8, 9 }));
    widget->position_grid = { -3, 4 };

    PropertyBinding binding;
    binding.target = WidgetPropertyType::VALUE;
    binding.event.channel = EventChannelId::Sensors;
    binding.event.event_type = 1;
    binding.event.payload_key = 2;
    binding.event.selector_key = 0;
    binding.event.selector_value = Text("rpm");
    binding.direction = PropertyBindingDirection::InOut;
    binding.outbound_event_type = 5;
    widget->bindings.push_back(binding);

    const auto bytes = ui_patch_records::EncodeWidget(*widget);
    const auto decoded = UiConfigurationCborParser::DecodeWidget(Mr(), bytes);

    zassert_not_null(decoded.get());
    zassert_true(IsSameWidget(*widget, *decoded));
    zassert_equal(decoded->position_grid.x, -3);
    zassert_equal(decoded->z_index, -2);

    // A widget without properties omits the map.
    const auto bare = MakeWidget(WidgetType::IndicatorBar, 1);
    zassert_true(IsSameWidget(*bare, *UiConfigurationCborParser::DecodeWidget(Mr(), ui_patch_records::EncodeWidget(*bare))));

    // Indefinite-length arrays, as zcbor writes them, read the same.
    auto indefinite = ui_patch_records::EncodeWidget(*bare);
    indefinite.front() = 0x9F;
    indefinite.push_back(0xFF);
    zassert_true(IsSameWidget(*bare, *UiConfigurationCborParser::DecodeWidget(Mr(), indefinite)));
}

ZTEST(ui_patch, test_a_screen_record_decodes_to_the_screen_it_was_written_from) {
    auto screen = MakeScreen(4, 9);
    screen->z_index = 2;
    screen->is_visible = false;
    screen->is_overlay = true;
    screen->type = ScreenType::Popup;
    screen->AddWidget(MakeWidget(WidgetType::IndicatorBar, 1, 1));
    auto digital = MakeWidget(WidgetType::IndicatorDigital, 2, 0);
    digital->properties.emplace(WidgetPropertyType::VALUE_PRECISION, 1);
    screen->AddWidget(digital);

    const auto decoded = UiConfigurationCborParser::DecodeScreen(Mr(), ui_patch_records::EncodeScreen(*screen));

    zassert_not_null(decoded.get());
    zassert_equal(ClassifyScreenChange(*screen, *decoded), WidgetChange::InPlace);
    zassert_equal(decoded->id, 4U);
    zassert_equal(decoded->screen_group_id, 9U);
    zassert_true(decoded->is_overlay);
    zassert_equal(decoded->grid.width, 10U);
    zassert_equal(decoded->widget_configurations.size(), 2U);
    zassert_equal(decoded->widget_configurations[0]->id, 2U, "Ordered by z-index, as a configuration is");
    zassert_true(IsSameWidget(*decoded->widget_configurations[0], *digital));
}

ZTEST(ui_patch, test_bytes_that_are_not_exactly_one_record_are_refused) {
    const auto widget = MakeWidget(WidgetType::IndicatorBar, 1);
    const auto good = ui_patch_records::EncodeWidget(*widget);

    const auto refuses_widget = [](const std::vector<uint8_t>& bytes) {
        try {
            (void)UiConfigurationCborParser::DecodeWidget(Mr(), bytes);
        } catch(const std::invalid_argument&) {
            return true;
        }

        return false;
    };

    // A second record, a stray byte, a cut-off record, nothing, noise and a screen are not a widget.
    auto twice = good;
    twice.insert(twice.end(), good.begin(), good.end());
    zassert_true(refuses_widget(twice));

    auto trailing = good;
    trailing.push_back(0x00);
    zassert_true(refuses_widget(trailing));

    zassert_true(refuses_widget(std::vector<uint8_t>(good.begin(), good.end() - 1)));
    zassert_true(refuses_widget({}));
    zassert_true(refuses_widget(std::vector<uint8_t>(20, 0xFF)));

    auto screen = MakeScreen(1, 1);
    screen->AddWidget(MakeWidget(WidgetType::IndicatorBar, 1));
    zassert_true(refuses_widget(ui_patch_records::EncodeScreen(*screen)));

    // And a widget is not a screen.
    bool is_refused = false;
    try {
        (void)UiConfigurationCborParser::DecodeScreen(Mr(), good);
    } catch(const std::invalid_argument&) {
        is_refused = true;
    }
    zassert_true(is_refused);

    // A record that closes the wrapper early and carries another behind it.
    is_refused = false;
    auto sneaky = good;
    sneaky.push_back(0x80);
    sneaky.push_back(0x80);
    try {
        (void)UiConfigurationCborParser::DecodeWidget(Mr(), sneaky);
    } catch(const std::invalid_argument&) {
        is_refused = true;
    }
    zassert_true(is_refused);

    // A property the unit does not know has the decoder's own message.
    auto unknown = MakeWidget(WidgetType::IndicatorBar, 1);
    unknown->properties.emplace(static_cast<WidgetPropertyType>(60), 1);
    std::string message;
    try {
        (void)UiConfigurationCborParser::DecodeWidget(Mr(), ui_patch_records::EncodeWidget(*unknown));
    } catch(const std::invalid_argument& e) {
        message = e.what();
    }
    zassert_true(message.contains("Unknown property ID: 60"), "'%s'", message.c_str());
}
