#include <array>
#include <memory>
#include <stdexcept>
#include <string>

#include <zephyr/ztest.h>

#include "domain/ui_domain/configuration/parsers/ui_configuration_cbor_parser.h"
#include "domain/ui_domain/configuration/parsers/ui_configuration_validator.h"
#include "domain/ui_domain/models/animation.h"
#include "domain/ui_domain/models/ui_configuration.h"
#include "domain/ui_domain/models/widget_composition.h"
#include "views/widgets/basic/arc_icon_widget/arc_icon_widget.h"
#include "views/widgets/indicators/dial_indicator/dial_indicator.h"
#include "views/widgets/widget_factory.h"

using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::domain::ui_domain::configuration::parsers;
using namespace eerie_leap::views::widgets;

// Preflight needs neither an LVGL display nor an event-channel registry.
ZTEST_SUITE(widget_composition, NULL, NULL, NULL, NULL, NULL);

namespace {

std::shared_ptr<WidgetConfiguration> Widget(uint32_t id, WidgetType type) {
    auto widget = std::make_shared<WidgetConfiguration>(std::allocator_arg, std::pmr::get_default_resource());
    widget->id = id;
    widget->type = type;
    widget->position_grid = { 0, 0 };
    widget->size_grid = { 1, 1 };
    return widget;
}

std::shared_ptr<ScreenConfiguration> Screen() {
    auto screen = std::make_shared<ScreenConfiguration>(std::allocator_arg, std::pmr::get_default_resource());
    screen->id = 42;
    screen->grid = { .width = 3, .height = 3 };
    return screen;
}

void Children(WidgetConfiguration& owner, std::initializer_list<int> ids) {
    owner.properties[WidgetPropertyType::CHILD_WIDGET_IDS] =
        std::pmr::vector<int>(ids, owner.properties.get_allocator().resource());
}

std::string CompositionError(const ScreenConfiguration& screen) {
    try {
        UiConfigurationValidator::Validate(screen, [](const auto& owner, auto children) {
            WidgetFactory::GetInstance().ValidateChildren(owner, children);
        });
    } catch(const std::invalid_argument& error) {
        return error.what();
    }
    return {};
}

void ExpectError(const ScreenConfiguration& screen, std::initializer_list<const char*> fragments) {
    const auto error = CompositionError(screen);
    zassert_false(error.empty(), "Expected a widget-owned contract rejection");
    zassert_true(error.find("Screen ID: 42") != std::string::npos, "%s", error.c_str());
    for(const auto* fragment : fragments)
        zassert_true(error.find(fragment) != std::string::npos, "Missing '%s' in '%s'", fragment, error.c_str());
}

// Test-only composite with two ordered roles and its own parent-local layout.
// Slot 0 is a digital readout and slot 1 is a button. No domain registry knows this.
class ReadoutAndButton : public basic::ArcIconWidget {
public:
    using ArcIconWidget::ArcIconWidget;

    static void ValidateChildren(const WidgetConfiguration&,
        std::span<const WidgetConfiguration* const> children) {
        if(children.size() != 2)
            throw std::invalid_argument("Expected 2 children in order: readout, button.");
        constexpr WidgetType types[] = { WidgetType::IndicatorDigital, WidgetType::ControlButton };
        constexpr const char* roles[] = { "readout", "button" };
        for(size_t i = 0; i < children.size(); ++i) {
            const auto prefix = "Child index " + std::to_string(i) + ", ID " + std::to_string(children[i]->id)
                + " (" + roles[i] + "): ";
            if(children[i]->type != types[i])
                throw std::invalid_argument(prefix + "unsupported configuration for this role.");
            if(children[i]->position_grid.x != static_cast<int>(i) || children[i]->position_grid.y != 0
                || children[i]->size_grid.width != 1 || children[i]->size_grid.height != 1)
                throw std::invalid_argument(prefix + "invalid parent-local placement.");
        }
    }
};

class TestRegistration {
public:
    int creation_count = 0;

    TestRegistration() {
        WidgetFactory::GetInstance().RegisterWidget(WidgetType::BasicArcIcon,
            [this](uint32_t, std::shared_ptr<Frame>, const WidgetContext&) -> std::unique_ptr<IWidget> {
                ++creation_count;
                throw std::runtime_error("Preflight must not construct widgets");
            }, ReadoutAndButton::ValidateChildren);
    }

    ~TestRegistration() {
        WidgetFactory::GetInstance().RegisterWidget<basic::ArcIconWidget>(WidgetType::BasicArcIcon);
    }
};

} // namespace

ZTEST(widget_composition, test_leaves_accept_empty_lists_and_reject_nonempty_children) {
    for(auto type : WidgetFactory::GetInstance().GetAvailableTypes()) {
        if(type == WidgetType::IndicatorDial)
            continue;
        auto screen = Screen();
        auto owner = Widget(9, type);
        screen->widget_configurations = { owner };
        zassert_true(CompositionError(*screen).empty());
        Children(*owner, {});
        zassert_true(CompositionError(*screen).empty());
        screen->widget_configurations.push_back(Widget(12, WidgetType::IndicatorDigital));
        Children(*owner, { 12 });
        ExpectError(*screen, { "Widget ID: 9", "expects 0 children", "child index 0, ID 12" });
    }
}

ZTEST(widget_composition, test_dial_requires_exactly_one_needle) {
    auto screen = Screen();
    auto dial = Widget(9, WidgetType::IndicatorDial);
    screen->widget_configurations = { dial };
    ExpectError(*screen, { "Widget ID: 9", "exactly 1 child", "index 0 is the needle", "received 0" });
    Children(*dial, {});
    ExpectError(*screen, { "Widget ID: 9", "exactly 1 child", "received 0" });
    screen->widget_configurations.push_back(Widget(12, WidgetType::IndicatorDigital));
    screen->widget_configurations.push_back(Widget(13, WidgetType::ControlButton));
    Children(*dial, { 12, 13 });
    ExpectError(*screen, { "Widget ID: 9", "exactly 1 child", "received 2" });
}

ZTEST(widget_composition, test_dial_accepts_every_widget_type_as_a_rotatable_needle) {
    auto dial = Widget(9, WidgetType::IndicatorDial);
    for(auto type : WidgetFactory::GetInstance().GetAvailableTypes()) {
        auto screen = Screen();
        auto needle = Widget(12, type);
        Children(*dial, { 12 });
        screen->widget_configurations = { dial, needle };
        if(type == WidgetType::IndicatorDial) {
            Children(*needle, { 13 });
            screen->widget_configurations.push_back(Widget(13, WidgetType::IndicatorDigital));
        }
        zassert_true(CompositionError(*screen).empty());
    }
}

ZTEST(widget_composition, test_dial_fill_slot_checks_outer_geometry_but_not_local_pixels_against_screen_grid) {
    auto screen = Screen();
    auto dial = Widget(9, WidgetType::IndicatorDial);
    auto needle = Widget(12, WidgetType::BasicIcon);
    Children(*dial, { 12 });
    needle->properties[WidgetPropertyType::ICON_TYPE] = static_cast<int>(IconType::TriangleIsosceles);
    needle->properties[WidgetPropertyType::WIDTH_PX] = 20;
    needle->properties[WidgetPropertyType::HEIGHT_PX] = 200;
    needle->properties[WidgetPropertyType::POSITION_X] = 30;
    needle->properties[WidgetPropertyType::POSITION_Y] = -10;
    needle->properties[WidgetPropertyType::ANCHOR_POINT_X] = 1000;
    needle->properties[WidgetPropertyType::ANCHOR_POINT_Y] = 7;
    screen->widget_configurations = { needle, dial };
    zassert_true(CompositionError(*screen).empty());
    for(auto position : { WidgetPosition { -1, 0 }, WidgetPosition { 0, 1 }, WidgetPosition { 100, 0 } }) {
        needle->position_grid = position;
        ExpectError(*screen, { "Widget ID: 9", "Child index 0, ID 12 (needle)", "position (0, 0)" });
    }
    needle->position_grid = { 0, 0 };
    for(auto size : { WidgetSize { 0, 1 }, WidgetSize { 1, 0 }, WidgetSize { 2, 1 }, WidgetSize { 1, 100 } }) {
        needle->size_grid = size;
        ExpectError(*screen, { "Widget ID: 9", "Child index 0, ID 12 (needle)", "size (1, 1)" });
    }
}

ZTEST(widget_composition, test_dial_rejects_rotation_animation_even_when_inactive) {
    auto screen = Screen();
    auto dial = Widget(9, WidgetType::IndicatorDial);
    auto needle = Widget(12, WidgetType::IndicatorDigital);
    Children(*dial, { 12 });
    screen->widget_configurations = { dial, needle };
    for(bool active : { false, true }) {
        needle->properties[WidgetPropertyType::IS_ANIMATION_ACTIVE] = active;
        needle->properties[WidgetPropertyType::ANIMATION_TYPE] = static_cast<int>(Animation::Type::Rotation);
        ExpectError(*screen, { "Widget ID: 9", "Child index 0, ID 12 (needle)", "rotation animation" });
    }
    needle->properties[WidgetPropertyType::ANIMATION_TYPE] = static_cast<int>(Animation::Type::Blinking);
    dial->properties[WidgetPropertyType::ANIMATION_TYPE] = static_cast<int>(Animation::Type::Rotation);
    zassert_true(CompositionError(*screen).empty()); // Independent ancestor rotation and needle blinking are supported.
}

ZTEST(widget_composition, test_dial_rejects_inbound_animation_type_bindings_and_allows_other_bindings) {
    auto screen = Screen();
    auto dial = Widget(9, WidgetType::IndicatorDial);
    auto needle = Widget(12, WidgetType::IndicatorDigital);
    Children(*dial, { 12 });
    screen->widget_configurations = { dial, needle };
    for(auto direction : { PropertyBindingDirection::In, PropertyBindingDirection::InOut }) {
        needle->bindings = { PropertyBinding {
            .target = WidgetPropertyType::ANIMATION_TYPE, .channel = EventChannelId::Sensors, .direction = direction
        } };
        ExpectError(*screen, { "Widget ID: 9", "Child index 0, ID 12 (needle)", "inbound ANIMATION_TYPE" });
    }
    needle->bindings[0].direction = PropertyBindingDirection::Out;
    zassert_true(CompositionError(*screen).empty());
    needle->bindings[0].direction = PropertyBindingDirection::In;
    for(auto target : { WidgetPropertyType::IS_ANIMATION_ACTIVE, WidgetPropertyType::ANIMATION_DURATION_MS,
        WidgetPropertyType::ANCHOR_POINT_X, WidgetPropertyType::VALUE }) {
        needle->bindings[0].target = target;
        zassert_true(CompositionError(*screen).empty());
    }
}

ZTEST(widget_composition, test_nested_composites_validate_each_owner_and_leave_configurations_unchanged) {
    auto screen = Screen();
    auto outer = Widget(9, WidgetType::IndicatorDial);
    auto inner = Widget(12, WidgetType::IndicatorDial);
    auto needle = Widget(3, WidgetType::ControlButton);
    Children(*outer, { 12 });
    Children(*inner, { 3 });
    outer->properties[WidgetPropertyType::ANCHOR_POINT_X] = 10;
    needle->properties[WidgetPropertyType::ANCHOR_POINT_X] = 25;
    screen->widget_configurations = { needle, outer, inner };
    const auto outer_properties = outer->properties;
    const auto needle_properties = needle->properties;
    zassert_true(CompositionError(*screen).empty());
    zassert_equal(screen->widget_configurations[0].get(), needle.get());
    zassert_equal(screen->widget_configurations[1].get(), outer.get());
    zassert_equal(screen->widget_configurations[2].get(), inner.get());
    zassert_true(outer->properties == outer_properties);
    zassert_true(needle->properties == needle_properties);
    Children(*inner, {});
    ExpectError(*screen, { "Widget ID: 12", "index 0 is the needle" });
}

ZTEST(widget_composition, test_multi_child_contract_preserves_semantic_order_and_never_constructs_widgets) {
    TestRegistration registration;
    auto screen = Screen();
    auto owner = Widget(9, WidgetType::BasicArcIcon);
    auto readout = Widget(20, WidgetType::IndicatorDigital);
    auto button = Widget(1, WidgetType::ControlButton);
    button->position_grid = { 1, 0 };
    button->z_index = -100;
    readout->z_index = 100;
    Children(*owner, { 20, 1 });
    // Child-local x=1 is valid for this composite even when the screen has only one grid column.
    screen->grid.width = 1;
    screen->widget_configurations = { button, owner, readout };
    zassert_true(CompositionError(*screen).empty());
    Children(*owner, { 1, 20 });
    ExpectError(*screen, { "Widget ID: 9", "Child index 0, ID 1 (readout)", "unsupported configuration" });
    Children(*owner, { 20 });
    button->position_grid = { 0, 0 }; // Now an unreferenced root, so keep its screen-grid geometry valid.
    ExpectError(*screen, { "Widget ID: 9", "Expected 2 children in order: readout, button" });
    Children(*owner, { 20, 1 });
    button->position_grid = { 0, 0 };
    ExpectError(*screen, { "Child index 1, ID 1 (button)", "parent-local placement" });
    zassert_equal(registration.creation_count, 0);
}

ZTEST(widget_composition, test_central_validator_rejects_invalid_structure_before_widget_rules) {
    auto screen = Screen();
    auto owner = Widget(9, WidgetType::IndicatorDial);
    auto child = Widget(12, static_cast<WidgetType>(255));
    Children(*owner, { 12 });
    screen->widget_configurations = { owner, child };
    int validation_count = 0;
    const auto validate_children = [&](const auto&, auto) { ++validation_count; };
    auto check = [&](const char* reason) {
        std::string message;
        try {
            UiConfigurationValidator::Validate(*screen, validate_children);
        } catch(const std::invalid_argument& error) {
            message = error.what();
        }
        zassert_true(message.find("Screen ID: 42") != std::string::npos, "%s", message.c_str());
        zassert_true(message.find(reason) != std::string::npos, "%s", message.c_str());
        zassert_equal(validation_count, 0);
    };
    check("Invalid widget type");
    screen->widget_configurations[1] = nullptr;
    check("null widget definition");
    screen->widget_configurations.pop_back();
    check("Child index 0, ID 12: target does not exist");
}

ZTEST(widget_composition, test_build_uses_validated_configuration_without_repeating_widget_rules) {
    auto screen = Screen();
    auto dial = Widget(9, WidgetType::IndicatorDial);
    auto needle = Widget(12, WidgetType::IndicatorDigital);
    Children(*dial, { 12 });
    screen->widget_configurations = { needle, dial };
    int dial_checks = 0;
    int needle_checks = 0;
    UiConfigurationValidator::Validate(*screen, [&](const auto& owner, auto children) {
        if(owner.id == 9) {
            ++dial_checks;
            zassert_equal(children.size(), 1);
            zassert_equal(children[0], needle.get());
        } else {
            ++needle_checks;
            zassert_true(children.empty());
        }
        WidgetFactory::GetInstance().ValidateChildren(owner, children);
    });
    const auto composition = WidgetComposition::Build(*screen);
    zassert_equal(dial_checks, 1);
    zassert_equal(needle_checks, 1);
    zassert_equal(composition.roots.size(), 1);
    zassert_equal(composition.roots[0], 1);
    zassert_equal(composition.GetChildren(1)[0], 0);
    zassert_equal(composition.postorder[0], 0);
    zassert_equal(composition.postorder[1], 1);
}

// A dial persisted before child composition kept its image on the dial itself.
ZTEST(widget_composition, test_a_legacy_dial_is_neither_saved_nor_loaded_under_widget_rules) {
    auto configuration = std::make_shared<UiConfiguration>(std::allocator_arg, std::pmr::get_default_resource());
    configuration->active_screen_group_id = 0;
    auto screen = Screen();
    screen->screen_group_id = 0;
    screen->type = ScreenType::Gauge;
    auto dial = Widget(9, WidgetType::IndicatorDial);
    dial->properties[WidgetPropertyType::FILE_PATH] = std::pmr::string("ui_img_arrow_al88.bin");
    dial->properties[WidgetPropertyType::IMG_WIDTH] = 15;
    dial->properties[WidgetPropertyType::IMG_HEIGHT] = 220;
    dial->properties[WidgetPropertyType::POSITION_Y] = -104;
    dial->properties[WidgetPropertyType::ANCHOR_POINT_X] = 7;
    dial->properties[WidgetPropertyType::ANCHOR_POINT_Y] = 7;
    screen->widget_configurations = { dial };
    configuration->screen_configurations.push_back(screen);

    // Domain rules alone keep it as stored: no implicit needle, moved image, or converted anchor.
    UiConfigurationCborParser domain_only;
    auto encoded = domain_only.Serialize(*configuration);
    auto decoded = domain_only.Deserialize(std::pmr::get_default_resource(), *encoded);
    const auto& definitions = decoded->screen_configurations[0]->widget_configurations;
    zassert_equal(definitions.size(), 1);
    zassert_false(definitions[0]->properties.contains(WidgetPropertyType::CHILD_WIDGET_IDS));
    zassert_true(definitions[0]->properties.contains(WidgetPropertyType::FILE_PATH));
    zassert_equal(std::get<int>(definitions[0]->properties.at(WidgetPropertyType::ANCHOR_POINT_Y)), 7);

    UiConfigurationCborParser with_widget_rules([](const auto& owner, auto children) {
        WidgetFactory::GetInstance().ValidateChildren(owner, children);
    });
    auto error_of = [](auto&& action) -> std::string {
        try {
            action();
        } catch(const std::invalid_argument& error) {
            return error.what();
        }
        return {};
    };
    for(const auto& error : {
            error_of([&] { with_widget_rules.Serialize(*configuration); }),
            error_of([&] { with_widget_rules.Deserialize(std::pmr::get_default_resource(), *encoded); }) }) {
        for(const auto* fragment : { "Screen ID: 42, Widget ID: 9", "exactly 1 child", "received 0" })
            zassert_true(error.find(fragment) != std::string::npos, "Missing '%s' in '%s'", fragment, error.c_str());
    }
}
