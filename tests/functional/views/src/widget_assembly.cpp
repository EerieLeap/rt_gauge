#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/sensor_domain/event_bus/sensor_events_channel.h"
#include "domain/ui_domain/configuration/parsers/ui_configuration_validator.h"
#include "domain/ui_domain/models/widget_composition.h"
#include "event_bus/event_channels.h"
#include "views/screens/screen.h"
#include "views/screens/widget_assembly.h"
#include "views/utilitites/grid_layout.h"
#include "views/widgets/basic/arc_icon_widget/arc_icon_widget.h"
#include "views/widgets/basic/icon_widget/icon_widget.h"
#include "views/widgets/indicators/digital_indicator/digital_indicator.h"
#include "views/widgets/widget_base.h"
#include "views/widgets/widget_factory.h"
#include "views_test_support.h"

using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::domain::sensor_domain::event_bus;
using namespace eerie_leap::views::widgets;
using eerie_leap::domain::ui_domain::configuration::parsers::UiConfigurationValidator;
using eerie_leap::event_bus::InitializeEventChannels;
using eerie_leap::views::screens::WidgetAssembly;
using eerie_leap::views::utilitites::GridLayout;

namespace {

struct Journal {
    std::vector<uint32_t> constructed;
    std::vector<uint32_t> configured;
    // A binding handler holds its widget's store, so expiry proves the subscription was released.
    std::vector<std::weak_ptr<WidgetPropertyStore>> stores;
    int live = 0;
    std::optional<uint32_t> fail_construction;
    std::optional<uint32_t> fail_configuration;
    std::optional<uint32_t> fail_render;
};

Journal journal;

class Node : public WidgetBase {
    WidgetType type_;

public:
    Node(uint32_t id, std::shared_ptr<Frame> parent, WidgetType type)
        : WidgetBase(id, std::move(parent), WidgetContext {}), type_(type) {
        if(journal.fail_construction == id)
            throw std::runtime_error("Construction failed");
        journal.constructed.push_back(id);
        journal.stores.push_back(properties_);
        ++journal.live;
    }

    ~Node() override {
        DetachDispatch();
        --journal.live;
    }

    WidgetType GetType() const override {
        return type_;
    }

    double Value() const {
        return properties_->GetAs<double>(WidgetPropertyType::VALUE, -1);
    }

    int ApplyTheme(const eerie_leap::views::themes::ITheme&) override {
        return 0;
    }

protected:
    int DoRender() override {
        return journal.fail_render == id_ ? -EIO : 0;
    }

    void RegisterProperties(WidgetPropertyStore& store) const override {
        WidgetBase::RegisterProperties(store);
        store.Register(WidgetPropertyType::VALUE, 0.0, PropertyChangeEffect::Repaint);
    }

    void OnConfigured() override {
        if(journal.fail_configuration == id_)
            throw std::runtime_error("Configuration failed");
        journal.configured.push_back(id_);
    }
};

// Accepts any number of children, which fill its mount.
class Group : public Node {
public:
    Group(uint32_t id, std::shared_ptr<Frame> parent) : Node(id, std::move(parent), WidgetType::BasicArcIcon) {}

    ~Group() override { DetachDispatch(); }

    static void ValidateChildren(const WidgetConfiguration&, std::span<const WidgetConfiguration* const>) {}

protected:
    void OnChildrenAttached(std::span<const std::unique_ptr<IWidget>>) override {}
};

// Index 0 is any content, index 1 a digital readout; each takes half of the mount.
class Pair : public Node {
    IWidget* left_ = nullptr;
    IWidget* right_ = nullptr;

public:
    Pair(uint32_t id, std::shared_ptr<Frame> parent) : Node(id, std::move(parent), WidgetType::BasicIcon) {}

    ~Pair() override { DetachDispatch(); }

    static void ValidateChildren(const WidgetConfiguration&, std::span<const WidgetConfiguration* const> children) {
        if(children.size() != 2 || children[1]->type != WidgetType::IndicatorDigital)
            throw std::invalid_argument("Expected 2 children in order: content, readout.");
    }

protected:
    void OnChildrenAttached(std::span<const std::unique_ptr<IWidget>> children) override {
        if(children.size() != 2 || children[1]->GetType() != WidgetType::IndicatorDigital)
            throw std::invalid_argument("Expected content and a digital readout, in that order.");
        left_ = children[0].get();
        right_ = children[1].get();
    }

    void LayoutChildren() override {
        const auto width = lv_obj_get_width(GetChildMount()->GetObject());
        const auto height = lv_obj_get_height(GetChildMount()->GetObject());
        left_->SetPositionPx({ 0, 0 });
        left_->SetSizePx({ static_cast<uint32_t>(width / 2), static_cast<uint32_t>(height) });
        right_->SetPositionPx({ width / 2, 0 });
        right_->SetSizePx({ static_cast<uint32_t>(width - width / 2), static_cast<uint32_t>(height) });
    }
};

class Registration {
public:
    Registration() {
        auto& factory = WidgetFactory::GetInstance();
        factory.RegisterWidget(WidgetType::IndicatorDigital,
            [](uint32_t id, std::shared_ptr<Frame> parent, const WidgetContext&) -> std::unique_ptr<IWidget> {
                return std::make_unique<Node>(id, std::move(parent), WidgetType::IndicatorDigital);
            });
        factory.RegisterWidget(WidgetType::BasicArcIcon,
            [](uint32_t id, std::shared_ptr<Frame> parent, const WidgetContext&) -> std::unique_ptr<IWidget> {
                return std::make_unique<Group>(id, std::move(parent));
            }, Group::ValidateChildren);
        factory.RegisterWidget(WidgetType::BasicIcon,
            [](uint32_t id, std::shared_ptr<Frame> parent, const WidgetContext&) -> std::unique_ptr<IWidget> {
                return std::make_unique<Pair>(id, std::move(parent));
            }, Pair::ValidateChildren);
    }

    ~Registration() {
        auto& factory = WidgetFactory::GetInstance();
        factory.RegisterWidget<indicators::DigitalIndicator>(WidgetType::IndicatorDigital);
        factory.RegisterWidget<basic::ArcIconWidget>(WidgetType::BasicArcIcon);
        factory.RegisterWidget<basic::IconWidget>(WidgetType::BasicIcon);
    }
};

std::optional<Registration> registration;

class CountingResource : public std::pmr::memory_resource {
public:
    size_t outstanding = 0;
    size_t peak = 0;

private:
    void* do_allocate(size_t bytes, size_t alignment) override {
        auto* pointer = std::pmr::get_default_resource()->allocate(bytes, alignment);
        outstanding += bytes;
        peak = std::max(peak, outstanding);
        return pointer;
    }

    void do_deallocate(void* pointer, size_t bytes, size_t alignment) override {
        std::pmr::get_default_resource()->deallocate(pointer, bytes, alignment);
        outstanding -= bytes;
    }

    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};

std::shared_ptr<ScreenConfiguration> Screen(std::pmr::memory_resource* resource = std::pmr::get_default_resource()) {
    auto screen = std::make_shared<ScreenConfiguration>(std::allocator_arg, resource);
    screen->id = 42;
    screen->grid = { .width = 3, .height = 3 };
    return screen;
}

std::shared_ptr<WidgetConfiguration> Add(ScreenConfiguration& screen, uint32_t id, WidgetType type,
    std::initializer_list<int> children = {}, int32_t z_index = 0) {
    auto widget = std::make_shared<WidgetConfiguration>(std::allocator_arg, std::pmr::get_default_resource());
    widget->id = id;
    widget->type = type;
    widget->z_index = z_index;
    widget->position_grid = { 0, 0 };
    widget->size_grid = { 1, 1 };
    if(children.size() != 0)
        widget->properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::pmr::vector<int>(children);
    widget->bindings.push_back(PropertyBinding {
        .target = WidgetPropertyType::VALUE, .channel = EventChannelId::Sensors,
        .event_type = std::to_underlying(SensorEventType::DataUpdated),
        .payload_key = std::to_underlying(SensorPayloadType::Value),
        .selector_key = std::to_underlying(SensorPayloadType::SensorId),
        .selector_value = static_cast<int>(id)
    });
    screen.widget_configurations.push_back(widget);
    return widget;
}

void Validate(const ScreenConfiguration& screen) {
    UiConfigurationValidator::Validate(screen, [](const auto& owner, auto children) {
        WidgetFactory::GetInstance().ValidateChildren(owner, children);
    });
}

WidgetAssembly Assemble(const std::shared_ptr<ScreenConfiguration>& screen, const std::shared_ptr<Frame>& container) {
    Validate(*screen);
    return WidgetAssembly::Assemble(screen, container, WidgetContext {});
}

std::shared_ptr<Frame> Container() {
    return std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(300, true).SetHeight(300, true).Build());
}

std::vector<uint32_t> Ids(std::span<const std::unique_ptr<IWidget>> widgets) {
    std::vector<uint32_t> ids;
    for(const auto& widget : widgets)
        ids.push_back(widget->GetId());
    return ids;
}

void Collect(const IWidget& widget, std::vector<const IWidget*>& nodes) {
    nodes.push_back(&widget);
    for(const auto& child : widget.GetChildren())
        Collect(*child, nodes);
}

const Node& AsNode(const IWidget& widget) {
    return dynamic_cast<const Node&>(widget);
}

lv_obj_t* Parent(const IWidget& widget) {
    return lv_obj_get_parent(widget.GetContainer()->GetObject());
}

int32_t DrawingIndex(const IWidget& widget) {
    return lv_obj_get_index(widget.GetContainer()->GetObject());
}

void Publish(uint32_t id, float value) {
    SensorEventsChannel::GetInstance().Publish({
        .source_id = 0, .type = SensorEventType::DataUpdated,
        .payload = { { SensorPayloadType::SensorId, id }, { SensorPayloadType::Value, value } }
    });
}

bool AllExpired(const std::vector<std::weak_ptr<WidgetPropertyStore>>& stores, size_t from = 0) {
    return std::all_of(stores.begin() + from, stores.end(), [](const auto& store) { return store.expired(); });
}

void* SetUp() {
    views_test::EnsureTestDisplay();
    InitializeEventChannels();
    return nullptr;
}

void Before(void*) {
    journal = Journal {};
    registration.emplace();
}

void After(void*) {
    registration.reset();
}

} // namespace

ZTEST_SUITE(widget_assembly, NULL, SetUp, Before, After, views_test::CleanTestDisplay);

ZTEST(widget_assembly, test_builds_each_definition_once_under_its_owner_mount_with_forward_references) {
    auto container = Container();
    auto screen = Screen();
    Add(*screen, 5, WidgetType::IndicatorDigital);
    Add(*screen, 1, WidgetType::BasicArcIcon, { 2, 7 });
    Add(*screen, 2, WidgetType::BasicIcon, { 0, 5 });
    Add(*screen, 0, WidgetType::IndicatorDigital);
    Add(*screen, 7, WidgetType::IndicatorDigital);
    Add(*screen, 9, WidgetType::IndicatorDigital);
    const auto definitions = std::vector(screen->widget_configurations.begin(), screen->widget_configurations.end());

    auto assembly = Assemble(screen, container);

    zassert_equal(journal.live, 6);
    // Siblings are created in drawing order: equal z-index keeps definition order (5 before 0).
    zassert_true((journal.constructed == std::vector<uint32_t> { 1, 9, 2, 7, 5, 0 }), "Owners precede descendants");
    zassert_true((journal.configured == std::vector<uint32_t> { 0, 5, 2, 7, 1, 9 }), "Descendants precede owners");
    const auto& roots = *assembly.GetRoots();
    zassert_true((Ids(roots) == std::vector<uint32_t> { 1, 9 }));
    zassert_equal(lv_obj_get_parent(assembly.GetFrame()->GetObject()), container->GetObject());
    zassert_equal(lv_obj_get_child_count(container->GetObject()), 1);
    zassert_equal(lv_obj_get_child_count(assembly.GetFrame()->GetObject()), 2);
    for(const auto& root : roots)
        zassert_equal(Parent(*root), assembly.GetFrame()->GetObject());

    const auto& group = *roots[0];
    zassert_true((Ids(group.GetChildren()) == std::vector<uint32_t> { 2, 7 }));
    const auto& pair = *group.GetChildren()[0];
    zassert_true((Ids(pair.GetChildren()) == std::vector<uint32_t> { 0, 5 }));
    for(const auto* owner : { &group, &pair }) {
        for(const auto& child : owner->GetChildren())
            zassert_equal(Parent(*child), owner->GetChildMount()->GetObject());
    }

    std::vector<const IWidget*> nodes;
    for(const auto& root : roots)
        Collect(*root, nodes);
    zassert_equal(nodes.size(), definitions.size());
    for(const auto& definition : definitions) {
        const auto found = std::count_if(nodes.begin(), nodes.end(), [&](const IWidget* node) {
            return node->GetId() == definition->id && node->GetConfiguration() == definition;
        });
        zassert_equal(found, 1, "Widget %u", definition->id);
    }
    zassert_true(std::equal(definitions.begin(), definitions.end(), screen->widget_configurations.begin()));
}

ZTEST(widget_assembly, test_z_index_orders_siblings_without_reordering_semantic_children) {
    auto container = Container();
    auto screen = Screen();
    Add(*screen, 8, WidgetType::IndicatorDigital, {}, 10);
    Add(*screen, 3, WidgetType::IndicatorDigital, {}, 5);
    Add(*screen, 1, WidgetType::BasicArcIcon, { 4, 2, 3 });
    Add(*screen, 4, WidgetType::IndicatorDigital, {}, 5);
    Add(*screen, 2, WidgetType::IndicatorDigital, {}, -1);
    Add(*screen, 9, WidgetType::IndicatorDigital, {}, -10);

    auto assembly = Assemble(screen, container);

    const auto& roots = *assembly.GetRoots();
    zassert_true((Ids(roots) == std::vector<uint32_t> { 8, 1, 9 }), "Roots keep definition order");
    zassert_equal(DrawingIndex(*roots[2]), 0);
    zassert_equal(DrawingIndex(*roots[1]), 1);
    zassert_equal(DrawingIndex(*roots[0]), 2);
    const auto children = roots[1]->GetChildren();
    zassert_true((Ids(children) == std::vector<uint32_t> { 4, 2, 3 }), "Children keep reference order");
    zassert_equal(DrawingIndex(*children[1]), 0);
    zassert_equal(DrawingIndex(*children[2]), 1, "Equal z-index keeps definition order");
    zassert_equal(DrawingIndex(*children[0]), 2);
}

ZTEST(widget_assembly, test_roots_use_the_screen_grid_and_children_use_owner_local_layout) {
    auto container = Container();
    auto screen = Screen();
    auto pair = Add(*screen, 1, WidgetType::BasicIcon, { 2, 3 });
    pair->position_grid = { 1, 1 };
    pair->size_grid = { 2, 1 };
    auto left = Add(*screen, 2, WidgetType::IndicatorDigital);
    left->position_grid = { 2, 2 }; // Would exceed the screen grid as a root; children never use it.
    left->size_grid = { 3, 3 };
    Add(*screen, 3, WidgetType::IndicatorDigital);
    Add(*screen, 4, WidgetType::BasicArcIcon, { 5 });
    Add(*screen, 5, WidgetType::IndicatorDigital);

    auto assembly = Assemble(screen, container);
    assembly.Commit();
    lv_obj_update_layout(container->GetObject());

    const auto layout = GridLayout::FromActiveScreen(screen->grid);
    const auto& roots = *assembly.GetRoots();
    for(size_t i = 0; i < roots.size(); ++i) {
        const auto& definition = *roots[i]->GetConfiguration();
        const auto size = layout.ToPx(definition.size_grid);
        const auto position = layout.ToPx(definition.position_grid, size);
        zassert_equal(roots[i]->GetSizePx().width, size.width);
        zassert_equal(roots[i]->GetSizePx().height, size.height);
        zassert_equal(roots[i]->GetPositionPx().x, position.x);
        zassert_equal(roots[i]->GetPositionPx().y, position.y);
    }
    const auto halves = roots[0]->GetChildren();
    const auto width = lv_obj_get_width(roots[0]->GetChildMount()->GetObject());
    zassert_equal(halves[0]->GetSizePx().width, static_cast<uint32_t>(width / 2));
    zassert_equal(halves[1]->GetPositionPx().x, width / 2);
    const auto& filler = *roots[1]->GetChildren()[0];
    zassert_equal(filler.GetSizePx().width, 0, "Child grid geometry is never converted against the screen");
    zassert_equal(lv_obj_get_width(filler.GetContainer()->GetObject()),
        lv_obj_get_width(roots[1]->GetChildMount()->GetObject()));
    zassert_equal(lv_obj_get_height(filler.GetContainer()->GetObject()),
        lv_obj_get_height(roots[1]->GetChildMount()->GetObject()));
}

ZTEST(widget_assembly, test_staged_tree_stays_hidden_and_unprocessed_until_commit) {
    auto container = Container();
    auto screen = Screen();
    Add(*screen, 1, WidgetType::BasicArcIcon, { 2 });
    Add(*screen, 2, WidgetType::IndicatorDigital);

    auto assembly = Assemble(screen, container);
    auto& root = *(*assembly.GetRoots())[0];
    const auto& child = AsNode(*root.GetChildren()[0]);
    zassert_equal(root.Render(), 0);
    root.OnActivated();

    zassert_true(lv_obj_has_flag(assembly.GetFrame()->GetObject(), LV_OBJ_FLAG_HIDDEN));
    zassert_false(AsNode(root).IsProcessingEligible());
    zassert_false(child.IsProcessingEligible());
    zassert_false(Frame::IsVisibleInHierarchy(views_test::WidgetContent(child)));
    Publish(2, 42.0F);
    zassert_equal(child.Value(), 42.0, "Staged widgets retain inbound values like any hidden widget");

    assembly.Commit();
    zassert_false(lv_obj_has_flag(assembly.GetFrame()->GetObject(), LV_OBJ_FLAG_HIDDEN));
    zassert_true(AsNode(root).IsProcessingEligible());
    zassert_true(child.IsProcessingEligible());
    zassert_true(Frame::IsVisibleInHierarchy(views_test::WidgetContent(child)));
}

ZTEST(widget_assembly, test_failures_release_every_staged_object_and_preserve_the_previous_tree) {
    auto container = Container();
    auto previous_screen = Screen();
    Add(*previous_screen, 1, WidgetType::BasicArcIcon, { 2 });
    Add(*previous_screen, 2, WidgetType::IndicatorDigital);
    auto previous = Assemble(previous_screen, container);
    previous.Commit();
    auto& previous_root = *(*previous.GetRoots())[0];
    zassert_equal(previous_root.Render(), 0);
    previous_root.OnActivated();
    const auto& previous_child = AsNode(*previous_root.GetChildren()[0]);

    auto candidate = Screen();
    Add(*candidate, 10, WidgetType::BasicArcIcon, { 11, 12 });
    Add(*candidate, 11, WidgetType::IndicatorDigital);
    auto pair = Add(*candidate, 12, WidgetType::BasicIcon, { 13, 14 });
    Add(*candidate, 13, WidgetType::BasicArcIcon);
    Add(*candidate, 14, WidgetType::IndicatorDigital);
    Add(*candidate, 15, WidgetType::IndicatorDigital);
    Validate(*candidate);

    const auto display_callbacks = lv_display_get_event_count(lv_display_get_default());
    auto expect_rollback = [&](const char* mode) {
        const auto first_store = journal.stores.size();
        bool failed = false;
        try {
            WidgetAssembly::Assemble(candidate, container, WidgetContext {});
        } catch(const std::exception&) {
            failed = true;
        }
        zassert_true(failed, "%s", mode);
        zassert_true(journal.stores.size() > first_store, "%s constructed nothing", mode);
        zassert_true(AllExpired(journal.stores, first_store), "%s left a widget or subscription", mode);
        zassert_equal(journal.live, 2, "%s", mode);
        zassert_equal(lv_obj_get_child_count(container->GetObject()), 1, "%s", mode);
        zassert_equal(lv_display_get_event_count(lv_display_get_default()), display_callbacks, "%s", mode);
        zassert_equal(previous.GetRoots()->size(), 1, "%s", mode);
        zassert_false(lv_obj_has_flag(previous.GetFrame()->GetObject(), LV_OBJ_FLAG_HIDDEN), "%s", mode);
    };

    journal.fail_construction = 14;
    expect_rollback("Descendant construction failure");
    journal.fail_construction.reset();
    journal.fail_configuration = 13;
    expect_rollback("Descendant configuration failure");
    journal.fail_configuration = 10;
    expect_rollback("Owner configuration failure after children subscribed");
    journal.fail_configuration.reset();
    // Unchanged-after-validation is a precondition; the receiving widget still enforces its order.
    pair->properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::pmr::vector<int> { 14, 13 };
    expect_rollback("Runtime child-order rejection");

    Publish(2, 64.0F);
    zassert_equal(previous_child.Value(), 64.0);
    zassert_true(previous_child.IsProcessingEligible());
}

ZTEST(widget_assembly, test_destruction_and_move_assignment_release_each_tree_once) {
    auto container = Container();
    const auto display_callbacks = lv_display_get_event_count(lv_display_get_default());
    auto screen = Screen();
    Add(*screen, 1, WidgetType::BasicArcIcon, { 2, 3 });
    Add(*screen, 2, WidgetType::IndicatorDigital);
    Add(*screen, 3, WidgetType::IndicatorDigital);
    Add(*screen, 4, WidgetType::IndicatorDigital);

    std::optional<WidgetAssembly> tree(Assemble(screen, container));
    tree->Commit();
    const auto roots = tree->GetRoots();
    std::weak_ptr<Frame> frame = tree->GetFrame();
    zassert_equal(journal.live, 4);

    const auto first_stores = journal.stores.size();
    *tree = Assemble(screen, container);
    zassert_equal(journal.live, 4, "The replaced tree is destroyed");
    zassert_true(AllExpired(std::vector(journal.stores.begin(), journal.stores.begin() + first_stores)));
    zassert_true(roots->empty(), "Retained root lists are emptied rather than outliving their frame");
    zassert_true(frame.expired());
    zassert_equal(lv_obj_get_child_count(container->GetObject()), 1);

    WidgetAssembly moved(std::move(*tree));
    tree.reset();
    zassert_equal(journal.live, 4, "A moved-from assembly owns nothing");
    frame = moved.GetFrame();
    {
        auto released = std::move(moved);
    }
    zassert_equal(journal.live, 0);
    zassert_true(AllExpired(journal.stores));
    zassert_true(frame.expired());
    zassert_equal(lv_obj_get_child_count(container->GetObject()), 0);
    zassert_equal(lv_display_get_event_count(lv_display_get_default()), display_callbacks);
}

ZTEST(widget_assembly, test_staging_metadata_is_bounded_and_released_at_the_graph_limits) {
    // Screen-scoped graph metadata only; widget instances and LVGL objects use their own heaps.
    constexpr size_t staging_budget = 4096;
    auto container = Container();
    CountingResource resource;
    auto screen = Screen(&resource);
    screen->widget_configurations.reserve(WidgetComposition::max_nodes);
    for(uint32_t id = 1; id < 7; ++id)
        Add(*screen, id, WidgetType::BasicArcIcon, { static_cast<int>(id + 1) });
    auto last_owner = Add(*screen, 7, WidgetType::BasicArcIcon);
    std::pmr::vector<int> leaves;
    for(uint32_t id = 8; id <= WidgetComposition::max_nodes; ++id) {
        Add(*screen, id, WidgetType::IndicatorDigital);
        leaves.push_back(static_cast<int>(id));
    }
    last_owner->properties[WidgetPropertyType::CHILD_WIDGET_IDS] = leaves;
    zassert_equal(screen->widget_configurations.size(), WidgetComposition::max_nodes);
    Validate(*screen);

    const auto baseline = resource.outstanding;
    resource.peak = baseline;
    auto assembly = WidgetAssembly::Assemble(screen, container, WidgetContext {});

    zassert_equal(resource.outstanding, baseline, "Staging metadata is released once the tree is assembled");
    zassert_true(resource.peak - baseline <= staging_budget, "Staging peak %zu", resource.peak - baseline);
    zassert_equal(static_cast<size_t>(journal.live), WidgetComposition::max_nodes);
    const IWidget* owner = (*assembly.GetRoots())[0].get();
    for(int depth = 1; depth < 7; ++depth)
        owner = owner->GetChildren()[0].get();
    zassert_equal(owner->GetId(), 7);
    zassert_equal(owner->GetChildren().size(), WidgetComposition::max_nodes - 7);
}

ZTEST(widget_assembly, test_screen_replaces_its_tree_only_after_the_candidate_assembles) {
    auto container = Container();
    eerie_leap::views::screens::Screen screen(42, container, WidgetContext {});
    auto first = Screen();
    Add(*first, 1, WidgetType::BasicArcIcon, { 2 });
    Add(*first, 2, WidgetType::IndicatorDigital);
    Add(*first, 3, WidgetType::IndicatorDigital);
    Validate(*first);
    screen.Configure(first);
    const auto roots = screen.GetWidgets();
    zassert_true((Ids(*roots) == std::vector<uint32_t> { 1, 3 }), "Referenced children are not roots");
    zassert_equal(Parent(*(*roots)[0]->GetChildren()[0]), (*roots)[0]->GetChildMount()->GetObject());
    zassert_equal(journal.live, 3);
    auto* screen_object = screen.GetContainer()->GetObject();
    zassert_equal(lv_obj_get_child_count(screen_object), 1);

    auto rejected = [&](std::shared_ptr<ScreenConfiguration> candidate, const char* mode) {
        const auto constructed = journal.constructed.size();
        bool failed = false;
        try {
            screen.Configure(std::move(candidate));
        } catch(const std::exception&) {
            failed = true;
        }
        zassert_true(failed, "%s", mode);
        zassert_true(journal.constructed.size() > constructed, "%s failed before assembly", mode);
        zassert_equal(screen.GetWidgets(), roots, "%s", mode);
        zassert_equal(roots->size(), 2, "%s", mode);
        zassert_equal(screen.GetConfiguration(), first, "%s", mode);
        zassert_equal(journal.live, 3, "%s", mode);
        zassert_equal(lv_obj_get_child_count(screen_object), 1, "%s", mode);
    };
    // The screen does not repeat preflight; the receiving widget still enforces its count.
    auto miscounted = Screen();
    Add(*miscounted, 10, WidgetType::BasicIcon, { 11 });
    Add(*miscounted, 11, WidgetType::IndicatorDigital);
    rejected(miscounted, "Widget-owned child count at injection");
    auto runtime = Screen();
    Add(*runtime, 10, WidgetType::BasicArcIcon, { 11 });
    Add(*runtime, 11, WidgetType::IndicatorDigital);
    Validate(*runtime);
    journal.fail_configuration = 10;
    rejected(runtime, "Runtime configuration failure");
    journal.fail_configuration.reset();

    screen.Configure(runtime);
    zassert_true(roots->empty(), "The previous tree is released after replacement");
    zassert_true((Ids(*screen.GetWidgets()) == std::vector<uint32_t> { 10 }));
    zassert_equal(screen.GetConfiguration(), runtime);
    zassert_equal(journal.live, 2);
    zassert_equal(lv_obj_get_child_count(screen_object), 1);
}

ZTEST(widget_assembly, test_screen_render_reports_failures_without_skipping_other_roots) {
    auto container = Container();
    eerie_leap::views::screens::Screen screen(42, container, WidgetContext {});
    auto configuration = Screen();
    Add(*configuration, 1, WidgetType::BasicArcIcon, { 2 });
    Add(*configuration, 2, WidgetType::IndicatorDigital);
    Add(*configuration, 3, WidgetType::IndicatorDigital);
    Validate(*configuration);
    screen.Configure(configuration);
    const auto& roots = *screen.GetWidgets();

    journal.fail_render = 2;
    zassert_equal(screen.Render(), -EIO);
    zassert_false(screen.IsReady());
    zassert_false(roots[0]->IsReady(), "A composite with a failed child is not ready");
    zassert_true(roots[1]->IsReady(), "Later roots still render");

    journal.fail_render.reset();
    zassert_equal(screen.Render(), 0);
    zassert_true(screen.IsReady());
    zassert_true(roots[0]->IsReady());
}
