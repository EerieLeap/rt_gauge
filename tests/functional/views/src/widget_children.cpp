#include <algorithm>
#include <array>
#include <cerrno>
#include <memory>
#include <stdexcept>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/sensor_domain/event_bus/sensor_events_channel.h"
#include "event_bus/event_channels.h"
#include "views/themes/default_theme.h"
#include "views/widgets/widget_base.h"
#include "views/widgets/basic/icon_widget/icon_widget.h"
#include "views_test_support.h"

using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::domain::sensor_domain::event_bus;
using namespace eerie_leap::views::widgets;
using eerie_leap::event_bus::InitializeEventChannels;
using eerie_leap::subsys::event_bus::EventData;
using eerie_leap::views::themes::DefaultTheme;
using eerie_leap::views::themes::ThemeManager;

namespace {

struct Observations {
    int renders = 0;
    int themes = 0;
    int activated = 0;
    int deactivated = 0;
    int processing_updates = 0;
    int suspensions = 0;
    int values = 0;
    int destroyed = 0;
    bool processing = false;
    double value = 0;
};

class Leaf : public WidgetBase {
public:
    std::shared_ptr<Observations> observations = std::make_shared<Observations>();
    std::vector<uint32_t>* render_order = nullptr;
    bool fail_render = false;
    bool throw_render = false;

    Leaf(uint32_t id, std::shared_ptr<Frame> parent) : WidgetBase(id, std::move(parent), WidgetContext {}) {}

    ~Leaf() override {
        DetachDispatch();
        ++observations->destroyed;
    }

    WidgetType GetType() const override {
        return WidgetType::IndicatorDigital;
    }

    auto Store() const {
        return properties_;
    }

    void Apply(WidgetPropertyType property, ConfigValue value) {
        ScopedLvglLock lock;
        properties_->Set(property, value);
        NotifyPropertyChanged(property, value, properties_->GetEffect(property));
    }

    void OnActivated() override {
        ++observations->activated;
        WidgetBase::OnActivated();
    }

    void OnDeactivated() override {
        ++observations->deactivated;
        WidgetBase::OnDeactivated();
    }

    int ApplyTheme(const eerie_leap::views::themes::ITheme&) override {
        ++observations->themes;
        return 0;
    }

protected:
    int DoRender() override {
        ++observations->renders;
        if(render_order != nullptr)
            render_order->push_back(GetId());
        if(throw_render)
            throw std::runtime_error("Render failed");
        return fail_render ? -EIO : 0;
    }

    void RegisterProperties(WidgetPropertyStore& store) const override {
        WidgetBase::RegisterProperties(store);
        store.Register(WidgetPropertyType::VALUE, 0.0, PropertyChangeEffect::Repaint);
        if(GetType() == WidgetType::IndicatorDigital)
            store.Register(WidgetPropertyType::LABEL, std::pmr::string("leaf"), PropertyChangeEffect::Repaint);
    }

    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override {
        if(type == WidgetPropertyType::VALUE) {
            ++observations->values;
            observations->value = std::get<double>(value);
        } else
            WidgetBase::OnPropertyChanged(type, value);
    }

    void OnProcessingUpdated(bool enabled) override {
        ++observations->processing_updates;
        observations->processing = enabled;
    }

    void OnProcessingSuspended() override {
        ++observations->suspensions;
    }
};

class Pair : public Leaf {
    IWidget* left_ = nullptr;
    IWidget* right_ = nullptr;
public:
    using Leaf::Leaf;

    ~Pair() override { DetachDispatch(); }

    WidgetType GetType() const override {
        return WidgetType::BasicArcIcon;
    }

    IWidget& Left() const {
        return *left_;
    }

    IWidget& Right() const {
        return *right_;
    }

    std::vector<WidgetPropertyType> GetSupportedProperties() const override {
        auto properties = WidgetBase::GetSupportedProperties();
        properties.push_back(WidgetPropertyType::CHILD_WIDGET_IDS);
        return properties;
    }

protected:
    void OnChildrenAttached(std::span<const std::unique_ptr<IWidget>> children) override {
        if(children.size() != 2 || children[1]->GetType() != WidgetType::IndicatorDigital)
            throw std::invalid_argument("Expected left content and a right digital readout, in that order.");

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

    int DoRender() override {
        if(!left_->IsReady() || !right_->IsReady())
            return -EINVAL;

        return Leaf::DoRender();
    }
};

std::shared_ptr<Frame> Mount() {
    return std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(200, true).SetHeight(100, true).Build());
}

std::shared_ptr<WidgetConfiguration> Configuration(uint32_t id, WidgetType type = WidgetType::IndicatorDigital) {
    auto configuration = std::make_shared<WidgetConfiguration>(std::allocator_arg, std::pmr::get_default_resource());
    configuration->id = id;
    configuration->type = type;
    configuration->position_grid = { 0, 0 };
    configuration->size_grid = { 1, 1 };
    configuration->properties[WidgetPropertyType::VALUE] = static_cast<double>(id);
    for(auto target : { WidgetPropertyType::VALUE, WidgetPropertyType::IS_ACTIVE, WidgetPropertyType::IS_VISIBLE,
        WidgetPropertyType::OPACITY, WidgetPropertyType::ANCHOR_POINT_X, WidgetPropertyType::IS_ANIMATION_ACTIVE }) {
        configuration->bindings.push_back(PropertyBinding {
            .target = target, .channel = EventChannelId::Sensors,
            .event_type = std::to_underlying(SensorEventType::DataUpdated),
            .payload_key = std::to_underlying(SensorPayloadType::Value),
            .selector_key = std::to_underlying(SensorPayloadType::SensorId),
            .selector_value = static_cast<int>(id * 100 + static_cast<uint32_t>(target))
        });
    }
    return configuration;
}

std::unique_ptr<Leaf> MakeLeaf(uint32_t id, std::shared_ptr<Frame> mount, bool blink = false) {
    auto leaf = std::make_unique<Leaf>(id, mount);
    auto configuration = Configuration(id);
    if(blink) {
        configuration->properties[WidgetPropertyType::ANIMATION_TYPE] = static_cast<int>(Animation::Type::Blinking);
        configuration->properties[WidgetPropertyType::IS_ANIMATION_ACTIVE] = true;
    }
    leaf->Configure(configuration);
    return leaf;
}

std::shared_ptr<WidgetConfiguration> PairConfiguration(uint32_t id, int left, int right) {
    auto configuration = Configuration(id, WidgetType::BasicArcIcon);
    configuration->properties[WidgetPropertyType::CHILD_WIDGET_IDS] = std::pmr::vector<int> { left, right };
    return configuration;
}

void Inject(Pair& owner, std::unique_ptr<IWidget> left, std::unique_ptr<IWidget> right) {
    IWidget::Children children;
    children.push_back(std::move(left));
    children.push_back(std::move(right));
    owner.SetChildren(std::move(children));
}

struct Tree {
    std::shared_ptr<Frame> mount = Mount();
    std::unique_ptr<Pair> root = std::make_unique<Pair>(1, mount);
    Pair* branch;
    Leaf* first;
    Leaf* second;
    Leaf* last;

    explicit Tree(bool blink = false) {
        auto nested = std::make_unique<Pair>(2, root->GetChildMount());
        branch = nested.get();
        auto left = MakeLeaf(3, branch->GetChildMount(), blink);
        auto right = MakeLeaf(4, branch->GetChildMount(), blink);
        first = left.get();
        second = right.get();
        Inject(*branch, std::move(left), std::move(right));
        branch->Configure(PairConfiguration(2, 3, 4));
        auto trailing = MakeLeaf(5, root->GetChildMount(), blink);
        last = trailing.get();
        Inject(*root, std::move(nested), std::move(trailing));
        root->Configure(PairConfiguration(1, 2, 5));
    }
    auto Nodes() const { return std::array<Leaf*, 5> { root.get(), branch, first, second, last }; }
    void Start() {
        zassert_equal(root->Render(), 0);
        root->OnActivated();
    }
};

void Publish(uint32_t id, WidgetPropertyType target, const EventData& value) {
    SensorEventsChannel::GetInstance().Publish({
        .source_id = 0, .type = SensorEventType::DataUpdated,
        .payload = {
            { SensorPayloadType::SensorId, id * 100 + static_cast<uint32_t>(target) },
            { SensorPayloadType::Value, value }
        }
    });
}

template<typename Action>
void ExpectRejected(Action&& action) {
    bool rejected = false;
    try { action(); } catch(const std::logic_error&) { rejected = true; }
    zassert_true(rejected);
}

void* SetUp() {
    views_test::EnsureTestDisplay();
    InitializeEventChannels();
    return nullptr;
}

K_THREAD_STACK_DEFINE(children_publisher_stack, 4096);

} // namespace

ZTEST_SUITE(widget_children, NULL, SetUp, NULL, views_test::CleanTestDisplay, NULL);

ZTEST(widget_children, test_nested_ownership_preserves_instances_configuration_and_property_surfaces) {
    Tree tree;
    zassert_equal(tree.root->GetChildren().size(), 2);
    zassert_equal(tree.root->GetChildren()[0].get(), tree.branch);
    zassert_equal(&tree.root->Left(), tree.branch);
    zassert_equal(&tree.branch->Left(), tree.first);
    zassert_equal(&tree.root->Right(), tree.last);
    const auto parent_properties = tree.root->GetSupportedProperties();
    zassert_true(std::find(parent_properties.begin(), parent_properties.end(), WidgetPropertyType::LABEL)
        == parent_properties.end());
    zassert_equal(std::count(parent_properties.begin(), parent_properties.end(), WidgetPropertyType::CHILD_WIDGET_IDS), 1);
    for(auto* node : tree.Nodes()) {
        zassert_equal(node->GetConfiguration()->id, node->GetId());
        zassert_equal(node->GetConfiguration()->type, node->GetType());
        zassert_equal(node->Store()->GetAs<double>(WidgetPropertyType::VALUE, -1), node->GetId());
        zassert_false(node->Store()->IsRegistered(WidgetPropertyType::CHILD_WIDGET_IDS));
    }
    zassert_not_equal(tree.root->Store().get(), tree.first->Store().get());
    const auto first_configuration = tree.first->GetConfiguration();
    ExpectRejected([&] { tree.root->Configure(PairConfiguration(1, 2, 5)); });
    ExpectRejected([&] { tree.first->Configure(Configuration(3)); });
    zassert_equal(tree.first->GetConfiguration().get(), first_configuration.get());
}

ZTEST(widget_children, test_injection_rejects_invalid_instances_mounts_counts_and_late_replacement) {
    auto mount = Mount();
    Pair pair(1, mount);
    ExpectRejected([&] { pair.Configure(PairConfiguration(1, 2, 3)); });
    ExpectRejected([&] { pair.SetChildren({}); });
    IWidget::Children null_child;
    null_child.push_back(nullptr);
    ExpectRejected([&] { pair.SetChildren(std::move(null_child)); });
    IWidget::Children unconfigured;
    unconfigured.push_back(std::make_unique<Leaf>(2, pair.GetChildMount()));
    ExpectRejected([&] { pair.SetChildren(std::move(unconfigured)); });
    ExpectRejected([&] { Inject(pair, MakeLeaf(2, mount), MakeLeaf(3, pair.GetChildMount())); });
    ExpectRejected([&] { Inject(pair, MakeLeaf(2, pair.GetChildMount()), MakeLeaf(2, pair.GetChildMount())); });
    auto bad_identity = MakeLeaf(3, pair.GetChildMount());
    bad_identity->GetConfiguration()->id = 99;
    ExpectRejected([&] { Inject(pair, MakeLeaf(2, pair.GetChildMount()), std::move(bad_identity)); });
    Inject(pair, MakeLeaf(2, pair.GetChildMount()), MakeLeaf(3, pair.GetChildMount()));
    ExpectRejected([&] { pair.Configure(PairConfiguration(1, 3, 2)); });
    zassert_is_null(pair.GetConfiguration().get());
    pair.Configure(PairConfiguration(1, 2, 3));
    const auto* left = &pair.Left();
    ExpectRejected([&] { pair.SetChildren({}); });
    zassert_equal(&pair.Left(), left);
    Leaf leaf(6, mount);
    IWidget::Children children;
    children.push_back(MakeLeaf(7, leaf.GetChildMount()));
    ExpectRejected([&] { leaf.SetChildren(std::move(children)); });
}

ZTEST(widget_children, test_render_activation_refresh_and_theme_visit_each_node_once) {
    const auto callbacks = lv_display_get_event_count(lv_display_get_default());
    Tree tree;
    zassert_equal(lv_display_get_event_count(lv_display_get_default()), callbacks + 1);
    std::vector<uint32_t> order;
    for(auto* node : tree.Nodes())
        node->render_order = &order;
    tree.Start();
    const std::vector<uint32_t> expected { 3, 4, 2, 5, 1 };
    zassert_true(order == expected);
    for(auto* node : tree.Nodes()) {
        zassert_equal(node->observations->renders, 1);
        zassert_equal(node->observations->activated, 1);
        node->observations->processing_updates = 0;
        node->observations->themes = 0;
    }
    {
        ScopedLvglLock lock;
        lv_display_send_event(lv_display_get_default(), LV_EVENT_REFR_START, nullptr);
    }
    for(auto* node : tree.Nodes())
        zassert_equal(node->observations->processing_updates, 1, "Widget %u", node->GetId());
    ThemeManager::GetInstance().SetTheme(std::make_shared<DefaultTheme>());
    for(auto* node : tree.Nodes())
        zassert_equal(node->observations->themes, 1);
    tree.root->OnDeactivated();
    for(auto* node : tree.Nodes())
        zassert_equal(node->observations->deactivated, 1);
    tree.root->OnActivated();
    for(auto* node : tree.Nodes())
        zassert_equal(node->observations->activated, 2);
}

ZTEST(widget_children, test_bindings_and_anchor_updates_remain_child_local_without_duplicate_delivery) {
    Tree tree;
    tree.Start();
    for(auto* node : tree.Nodes())
        node->observations->values = 0;
    Publish(3, WidgetPropertyType::VALUE, 42.0F);
    zassert_equal(tree.first->observations->values, 1);
    zassert_equal(tree.first->observations->value, 42.0);
    zassert_equal(tree.root->observations->values, 0);
    zassert_equal(tree.branch->observations->values, 0);
    zassert_equal(tree.second->observations->values, 0);
    zassert_equal(tree.last->observations->values, 0);
    Publish(1, WidgetPropertyType::VALUE, 70.0F);
    zassert_equal(tree.root->observations->values, 1);
    zassert_equal(tree.first->observations->value, 42.0);
    zassert_true(tree.first->SetRotation(450));
    Publish(3, WidgetPropertyType::ANCHOR_POINT_X, 7);
    zassert_equal(lv_obj_get_style_transform_pivot_x(tree.first->GetChildMount()->GetObject(), LV_PART_MAIN), 7);
    zassert_equal(tree.root->Store()->GetAs<int>(WidgetPropertyType::ANCHOR_POINT_X, -2), -1);
    zassert_equal(lv_obj_get_style_transform_rotation(tree.first->GetChildMount()->GetObject(), LV_PART_MAIN), 450);
}

ZTEST(widget_children, test_parent_management_gates_descendants_and_preserves_child_local_state) {
    const auto animations = lv_anim_count_running();
    Tree tree(true);
    tree.Start();
    zassert_equal(lv_anim_count_running(), animations + 3);
    Publish(4, WidgetPropertyType::IS_VISIBLE, false);
    zassert_equal(lv_anim_count_running(), animations + 2);
    for(auto property : { WidgetPropertyType::IS_VISIBLE, WidgetPropertyType::OPACITY, WidgetPropertyType::IS_ACTIVE }) {
        const auto hidden = property == WidgetPropertyType::OPACITY ? EventData { 0 } : EventData { false };
        const auto shown = property == WidgetPropertyType::OPACITY ? EventData { 255 } : EventData { true };
        Publish(1, property, hidden);
        zassert_equal(lv_anim_count_running(), animations);
        for(auto* node : tree.Nodes())
            zassert_false(node->IsProcessingEligible());
        const auto before = tree.first->Store()->GetAs<double>(WidgetPropertyType::VALUE, -1);
        tree.first->observations->values = 0;
        Publish(3, WidgetPropertyType::VALUE, static_cast<float>(before + 10));
        zassert_equal(tree.first->observations->values, 0);
        Publish(1, property, shown);
        zassert_equal(lv_anim_count_running(), animations + 2);
        zassert_false(tree.second->IsVisible());
        const auto expected = property == WidgetPropertyType::IS_ACTIVE ? before : before + 10;
        zassert_equal(tree.first->observations->value, expected);
    }
    tree.root->OnDeactivated();
    zassert_equal(lv_anim_count_running(), animations);
    Publish(3, WidgetPropertyType::VALUE, 99.0F);
    tree.root->OnActivated();
    zassert_equal(tree.first->observations->value, 99.0);
    zassert_false(tree.second->IsVisible());
    zassert_equal(lv_anim_count_running(), animations + 2);
}

ZTEST(widget_children, test_parent_frame_gates_and_mount_resize_preserve_semantic_and_drawing_order) {
    Tree tree;
    tree.Start();
    tree.root->SetSizePx({ 240, 80 });
    zassert_equal(tree.branch->GetSizePx().width, 120);
    zassert_equal(tree.first->GetSizePx().width, 60);
    zassert_equal(tree.last->GetPositionPx().x, 120);
    auto* last_object = tree.last->GetContainer()->GetObject();
    {
        ScopedLvglLock lock;
        lv_obj_move_to_index(last_object, 0);
        lv_obj_add_flag(tree.mount->GetObject(), LV_OBJ_FLAG_HIDDEN);
        lv_display_send_event(lv_display_get_default(), LV_EVENT_REFR_START, nullptr);
    }
    zassert_false(tree.first->IsProcessingEligible());
    zassert_equal(&tree.root->Left(), tree.branch);
    zassert_equal(tree.root->GetChildren()[1].get(), tree.last);
    zassert_equal(tree.root->Render(), 0);
    zassert_equal(lv_obj_get_index(last_object), 0);
    {
        ScopedLvglLock lock;
        lv_obj_remove_flag(tree.mount->GetObject(), LV_OBJ_FLAG_HIDDEN);
        tree.mount->SetProcessingEnabled(false);
        lv_display_send_event(lv_display_get_default(), LV_EVENT_REFR_START, nullptr);
    }
    zassert_false(tree.last->IsProcessingEligible());
    tree.mount->SetProcessingEnabled(true);
    tree.root->Synchronize();
    zassert_true(tree.last->IsProcessingEligible());
}

ZTEST(widget_children, test_descendant_render_failure_propagates_and_recovery_keeps_instances) {
    const auto animations = lv_anim_count_running();
    Tree tree(true);
    tree.Start();
    tree.second->fail_render = true;
    const int root_renders = tree.root->observations->renders;
    const int last_renders = tree.last->observations->renders;
    zassert_equal(tree.root->Render(), -EIO);
    zassert_false(tree.root->IsReady());
    zassert_false(tree.branch->IsReady());
    zassert_false(tree.second->IsReady());
    zassert_equal(tree.root->observations->renders, root_renders);
    zassert_equal(tree.last->observations->renders, last_renders);
    zassert_equal(lv_anim_count_running(), animations);
    Publish(3, WidgetPropertyType::VALUE, 88.0F);
    tree.second->fail_render = false;
    zassert_equal(tree.root->Render(), 0);
    zassert_equal(tree.first->observations->value, 88.0);
    zassert_equal(lv_anim_count_running(), animations + 3);
    zassert_equal(&tree.branch->Right(), tree.second);
}

ZTEST(widget_children, test_teardown_detaches_entire_tree_before_waiting_delivery_and_deletes_frames_once) {
    const auto callbacks = lv_display_get_event_count(lv_display_get_default());
    const auto animations = lv_anim_count_running();
    Tree tree(true);
    tree.Start();
    const auto store = tree.first->Store();
    std::vector<std::shared_ptr<Observations>> observations;
    std::vector<std::weak_ptr<Frame>> frames;
    for(auto* node : tree.Nodes()) {
        observations.push_back(node->observations);
        frames.push_back(node->GetContainer());
    }
    k_thread publisher {};
    k_sem started {};
    k_sem_init(&started, 0, 1);
    {
        ScopedLvglLock lock;
        k_thread_create(&publisher, children_publisher_stack, K_THREAD_STACK_SIZEOF(children_publisher_stack),
            [](void* context, void*, void*) {
                k_sem_give(static_cast<k_sem*>(context));
                Publish(3, WidgetPropertyType::VALUE, 55.0F);
            }, &started, nullptr, nullptr, K_PRIO_COOP(0), 0, K_NO_WAIT);
        zassert_equal(k_sem_take(&started, K_MSEC(1000)), 0);
        tree.root.reset();
    }
    zassert_equal(k_thread_join(&publisher, K_MSEC(1000)), 0);
    zassert_equal(store->GetAs<double>(WidgetPropertyType::VALUE, -1), 3.0);
    for(const auto& observation : observations)
        zassert_equal(observation->destroyed, 1);
    for(const auto& frame : frames)
        zassert_true(frame.expired());
    zassert_equal(lv_obj_get_child_count(tree.mount->GetObject()), 0);
    zassert_equal(lv_display_get_event_count(lv_display_get_default()), callbacks);
    zassert_equal(lv_anim_count_running(), animations);
    ThemeManager::GetInstance().SetTheme(std::make_shared<DefaultTheme>());
    lv_refr_now(nullptr);
}

ZTEST(widget_children, test_render_exception_and_terminal_detach_stop_all_descendant_work) {
    const auto animations = lv_anim_count_running();
    Tree tree(true);
    tree.Start();
    tree.second->throw_render = true;
    bool failed = false;
    try { tree.root->Render(); } catch(const std::runtime_error&) { failed = true; }
    zassert_true(failed);
    zassert_false(tree.root->IsReady());
    zassert_equal(lv_anim_count_running(), animations);
    tree.second->throw_render = false;
    zassert_equal(tree.root->Render(), 0);
    zassert_equal(lv_anim_count_running(), animations + 3);
    tree.root->Detach();
    for(auto* node : tree.Nodes()) {
        zassert_false(node->IsReady());
        zassert_false(node->IsTrackingEligible());
        zassert_false(node->IsProcessingEligible());
        node->observations->themes = 0;
    }
    tree.root->Detach();
    tree.root->Synchronize();
    tree.root->OnActivated();
    zassert_equal(tree.root->Render(), -EINVAL);
    Publish(3, WidgetPropertyType::VALUE, 77.0F);
    zassert_equal(tree.first->Store()->GetAs<double>(WidgetPropertyType::VALUE, -1), 3.0);
    ThemeManager::GetInstance().SetTheme(std::make_shared<DefaultTheme>());
    for(auto* node : tree.Nodes())
        zassert_equal(node->observations->themes, 0);
    zassert_equal(lv_anim_count_running(), animations);
}

ZTEST(widget_children, test_owned_icon_retains_native_rotation_across_parent_rotation_resize_and_rerender) {
    auto mount = Mount();
    Pair pair(1, mount);
    auto icon = std::make_unique<basic::IconWidget>(2, pair.GetChildMount(), WidgetContext {}, IconType::TriangleIsosceles);
    auto* needle = icon.get();
    auto configuration = Configuration(2, WidgetType::BasicIcon);
    configuration->bindings.clear();
    configuration->properties.erase(WidgetPropertyType::VALUE);
    configuration->properties[WidgetPropertyType::WIDTH_PX] = 20;
    configuration->properties[WidgetPropertyType::HEIGHT_PX] = 50;
    configuration->properties[WidgetPropertyType::ANCHOR_POINT_X] = 7;
    configuration->properties[WidgetPropertyType::ANCHOR_POINT_Y] = 7;
    icon->Configure(configuration);
    zassert_true(icon->SetRotation(900));
    Inject(pair, std::move(icon), MakeLeaf(3, pair.GetChildMount()));
    pair.Configure(PairConfiguration(1, 2, 3));
    zassert_equal(pair.Render(), 0);
    pair.OnActivated();
    zassert_true(pair.SetRotation(450));
    pair.SetSizePx({ 300, 100 });
    zassert_equal(pair.Render(), 0);
    auto* image = needle->GetIconContainer()->GetObject();
    zassert_equal(lv_image_get_rotation(image), 900);
    lv_point_t pivot;
    lv_image_get_pivot(image, &pivot);
    zassert_equal(pivot.x, 7);
    zassert_equal(pivot.y, 43);
    zassert_equal(lv_obj_get_style_transform_rotation(pair.GetChildMount()->GetObject(), LV_PART_MAIN), 450);
    zassert_equal(&pair.Left(), needle);
    zassert_equal(needle->GetConfiguration().get(), configuration.get());
}
