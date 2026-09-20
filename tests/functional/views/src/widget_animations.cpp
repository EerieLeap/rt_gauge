#include <algorithm>
#include <array>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/sensor_domain/event_bus/sensor_events_channel.h"
#include "event_bus/event_channel_id.h"
#include "event_bus/event_channels.h"
#include "views/animations/view_animator.h"
#include "views/widgets/basic/icons/shape_icon/oval_icon/oval_icon.h"
#include "views/widgets/controls/button_control/button_control.h"
#include "views/widgets/controls/slider_control/slider_control.h"
#include "views/widgets/widget_animation.h"
#include "views/widgets/widget_base.h"

#include "views_test_support.h"

using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::domain::sensor_domain::event_bus;
using namespace eerie_leap::views::widgets;
using eerie_leap::domain::ui_domain::ScopedLvglLock;
using eerie_leap::event_bus::EventChannelId;
using eerie_leap::views::animations::ViewAnimator;
using eerie_leap::views::themes::ITheme;
using eerie_leap::views::utilitites::Frame;

namespace {

constexpr int extent = 96;

static_assert(!std::is_copy_constructible_v<ViewAnimator>);
static_assert(!std::is_move_constructible_v<ViewAnimator>);
static_assert(!std::is_copy_assignable_v<ViewAnimator>);
static_assert(!std::is_move_assignable_v<ViewAnimator>);

bool fail_next_animation_start = false;
uint32_t animation_start_calls = 0;

struct AnimatorScene {
    Frame layout = Frame::CreateWrapped().SetWidth(64, true).SetHeight(24, true).Build();
    Frame presentation = Frame::CreatePresentation(layout.GetObject()).Build();
    bool eligible = true;
    ViewAnimator animator;

    AnimatorScene() {
        lv_obj_update_layout(layout.GetObject());
        zassert_true(animator.Attach(presentation, layout, [](void* context) {
            return *static_cast<bool*>(context);
        }, &eligible));
    }

    int Opacity() { return lv_obj_get_style_opa_layered(presentation.GetObject(), LV_PART_MAIN); }
    int Angle() { return lv_obj_get_style_transform_rotation(presentation.GetObject(), LV_PART_MAIN); }
};

void Advance(uint32_t elapsed) {
    lv_tick_inc(elapsed);
    lv_anim_refr_now();
}

void OtherAnimation(void*, int32_t) {}

std::shared_ptr<WidgetConfiguration> Configuration() {
    auto configuration = std::make_shared<WidgetConfiguration>(std::allocator_arg, std::pmr::get_default_resource());
    configuration->bindings.push_back(PropertyBinding {
        .target = WidgetPropertyType::VALUE,
        .channel = EventChannelId::Sensors,
        .event_type = std::to_underlying(SensorEventType::DataUpdated),
        .payload_key = std::to_underlying(SensorPayloadType::Value)
    });
    return configuration;
}

class PresentationProbe : public WidgetBase {
public:
    using WidgetBase::WidgetBase;
    ~PresentationProbe() override { DetachDispatch(); }
    WidgetType GetType() const override { return WidgetType::BasicIcon; }
    std::shared_ptr<Frame> Content() const { return content_frame_; }
    double applied_value = 0;

protected:
    void RegisterProperties(WidgetPropertyStore& store) const override {
        WidgetBase::RegisterProperties(store);
        store.Register(WidgetPropertyType::VALUE, ConfigValue { 0.0 }, PropertyChangeEffect::None);
    }

    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override {
        if(type == WidgetPropertyType::VALUE)
            applied_value = ConfigValueAs<double>(value, 0.0);
        else
            WidgetBase::OnPropertyChanged(type, value);
    }

    int DoRender() override {
        for(int index = 0; index < 2; ++index) {
            auto* rectangle = lv_obj_create(content_frame_->GetObject());
            lv_obj_remove_style_all(rectangle);
            lv_obj_remove_flag(rectangle, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_set_pos(rectangle, index == 0 ? 2 : 42, index == 0 ? 2 : 12);
            lv_obj_set_size(rectangle, 20, index == 0 ? 8 : 10);
            lv_obj_set_style_bg_color(rectangle, lv_color_hex(index == 0 ? 0xFF0000 : 0x00FF00), 0);
            lv_obj_set_style_bg_opa(rectangle, LV_OPA_COVER, 0);
        }
        return 0;
    }

    int ApplyTheme(const ITheme&) override { return 0; }
};

class TestOval : public basic::icons::OvalIcon {
public:
    using OvalIcon::OvalIcon;
    using IconBase::IsProcessingEligible;
};

class LifecycleProbe : public WidgetBase {
public:
    using WidgetBase::WidgetBase;
    ~LifecycleProbe() override { DetachDispatch(); }
    WidgetType GetType() const override { return WidgetType::BasicIcon; }
    void Detach() { DetachDispatch(); }
    int render_result = 0;
    int theme_result = 0;
    double applied_value = 0;
    uint32_t starts_at_value = 0;
    std::vector<WidgetPropertyType> notified;

protected:
    void RegisterProperties(WidgetPropertyStore& store) const override {
        WidgetBase::RegisterProperties(store);
        store.Register(WidgetPropertyType::VALUE, ConfigValue { 0.0 }, PropertyChangeEffect::None);
    }
    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override {
        notified.push_back(type);
        if(type == WidgetPropertyType::VALUE) {
            applied_value = ConfigValueAs<double>(value, 0.0);
            starts_at_value = animation_start_calls;
        }
    }
    void OnProcessingUpdated(bool) override { }
    void OnProcessingSuspended() override { }
    int DoRender() override { return render_result; }
    int ApplyTheme(const ITheme&) override { return theme_result; }
};

std::shared_ptr<WidgetConfiguration> AnimationConfiguration(Animation::Type type = Animation::Type::Rotation) {
    auto configuration = Configuration();
    configuration->bindings.clear();
    configuration->properties[WidgetPropertyType::ANIMATION_TYPE] = static_cast<int>(type);
    configuration->properties[WidgetPropertyType::IS_ANIMATION_ACTIVE] = true;
    configuration->properties[WidgetPropertyType::ANIMATION_DURATION_MS] = 1000;
    for(auto target : { WidgetPropertyType::ANIMATION_TYPE, WidgetPropertyType::IS_ANIMATION_ACTIVE,
                       WidgetPropertyType::ANIMATION_DURATION_MS, WidgetPropertyType::IS_ACTIVE,
                       WidgetPropertyType::IS_VISIBLE, WidgetPropertyType::OPACITY, WidgetPropertyType::VALUE,
                       WidgetPropertyType::COLOR_PRIMARY_ACTIVE }) {
        configuration->bindings.push_back(PropertyBinding {
            .target = target,
            .channel = EventChannelId::Sensors,
            .event_type = std::to_underlying(SensorEventType::DataUpdated),
            .payload_key = std::to_underlying(SensorPayloadType::Value),
            .selector_key = std::to_underlying(SensorPayloadType::SensorId),
            .selector_value = static_cast<int>(target)
        });
    }
    return configuration;
}

void Publish(WidgetPropertyType target, const eerie_leap::subsys::event_bus::EventData& value) {
    SensorEventsChannel::GetInstance().Publish({
        .source_id = 0,
        .type = SensorEventType::DataUpdated,
        .payload = {{ SensorPayloadType::SensorId, static_cast<int>(target) }, { SensorPayloadType::Value, value }}
    });
}

void Refresh() {
    lv_display_send_event(lv_display_get_default(), LV_EVENT_REFR_START, nullptr);
}

class TestButton : public controls::ButtonControl {
public:
    using ButtonControl::ButtonControl;
    ~TestButton() override { DetachDispatch(); }
    int clicks = 0;

protected:
    void OnControlEvent(lv_event_code_t code) override {
        ButtonControl::OnControlEvent(code);
        if(code == LV_EVENT_CLICKED)
            ++clicks;
    }
};

class Pointer {
    lv_indev_t* device_ = lv_indev_create();
    lv_point_t point_{};
    lv_indev_state_t state_ = LV_INDEV_STATE_RELEASED;

    static void Read(lv_indev_t* device, lv_indev_data_t* data) {
        auto* self = static_cast<Pointer*>(lv_indev_get_user_data(device));
        data->point = self->point_;
        data->state = self->state_;
    }

public:
    Pointer() {
        lv_indev_set_type(device_, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(device_, lv_display_get_default());
        lv_indev_set_mode(device_, LV_INDEV_MODE_EVENT);
        lv_indev_set_user_data(device_, this);
        lv_indev_set_read_cb(device_, Read);
        lv_indev_set_gesture_min_velocity(device_, 3);
        lv_indev_set_gesture_min_distance(device_, 5);
    }

    ~Pointer() { lv_indev_delete(device_); }

    void Send(int column, int row, lv_indev_state_t state) {
        point_ = { column, row };
        state_ = state;
        lv_indev_read(device_);
    }

    void Click(int column, int row) {
        Send(column, row, LV_INDEV_STATE_PRESSED);
        Send(column, row, LV_INDEV_STATE_RELEASED);
    }
};

class PresentationStyles {
    lv_obj_t* layout_;
    lv_obj_t* content_;
    bool layout_overflow_;
    bool content_overflow_;

    static void DrawMargin(lv_event_t* event) {
        auto* self = static_cast<PresentationStyles*>(lv_event_get_user_data(event));
        if(lv_obj_get_style_transform_rotation(self->content_, LV_PART_MAIN) == 0)
            return;
        lv_area_t bounds;
        lv_area_t layout;
        lv_obj_get_coords(self->content_, &bounds);
        lv_obj_get_transformed_area(self->content_, &bounds, LV_OBJ_POINT_TRANSFORM_FLAG_NONE);
        lv_obj_get_coords(self->layout_, &layout);
        const auto margin = std::max({ 0, layout.x1 - bounds.x1, layout.y1 - bounds.y1,
            bounds.x2 - layout.x2, bounds.y2 - layout.y2 });
        lv_event_set_ext_draw_size(event, margin + 1);
    }

public:
    explicit PresentationStyles(const IWidget& widget)
        : layout_(widget.GetContainer()->GetObject()), content_(views_test::WidgetContent(widget)),
          layout_overflow_(lv_obj_has_flag(layout_, LV_OBJ_FLAG_OVERFLOW_VISIBLE)),
          content_overflow_(lv_obj_has_flag(content_, LV_OBJ_FLAG_OVERFLOW_VISIBLE)) {
        lv_obj_add_event_cb(layout_, DrawMargin, LV_EVENT_REFR_EXT_DRAW_SIZE, this);
    }

    ~PresentationStyles() {
        Reset();
        lv_obj_remove_event_cb_with_user_data(layout_, DrawMargin, this);
    }

    void Rotate(int32_t angle) {
        lv_obj_add_flag(layout_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_add_flag(content_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_set_style_transform_rotation(content_, angle, LV_PART_MAIN);
        lv_obj_refresh_ext_draw_size(layout_);
    }

    void Fade(lv_opa_t opacity) { lv_obj_set_style_opa_layered(content_, opacity, LV_PART_MAIN); }

    void Reset() {
        lv_obj_set_style_transform_rotation(content_, 0, LV_PART_MAIN);
        lv_obj_set_style_opa_layered(content_, LV_OPA_COVER, LV_PART_MAIN);
        if(!layout_overflow_)
            lv_obj_remove_flag(layout_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        if(!content_overflow_)
            lv_obj_remove_flag(content_, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
        lv_obj_refresh_ext_draw_size(layout_);
    }
};

struct Scene {
    std::shared_ptr<Frame> root = std::make_shared<Frame>(
        Frame::CreateWrapped().SetWidth(extent, true).SetHeight(extent, true).Build());
    std::shared_ptr<Frame> clip = std::make_shared<Frame>(
        Frame::CreateWrapped(root->GetObject()).SetWidth(extent, true).SetHeight(extent, true).Build());
    PresentationProbe widget { 1, clip, WidgetContext{} };

    Scene() {
        lv_obj_set_style_bg_color(root->GetObject(), lv_color_black(), 0);
        lv_obj_set_style_bg_opa(root->GetObject(), LV_OPA_COVER, 0);
        widget.SetPositionPx({ 16, 36 });
        widget.SetSizePx({ 64, 24 });
        widget.Configure(Configuration());
        zassert_equal(widget.Render(), 0);
        widget.OnActivated();
        lv_obj_update_layout(root->GetObject());
    }

    std::vector<lv_color32_t> Pixels() const {
        lv_obj_update_layout(root->GetObject());
        std::unique_ptr<lv_draw_buf_t, decltype(&lv_draw_buf_destroy)> snapshot(
            lv_snapshot_take(root->GetObject(), LV_COLOR_FORMAT_ARGB8888), lv_draw_buf_destroy);
        zassert_not_null(snapshot);
        zassert_equal(snapshot->header.w, extent);
        zassert_equal(snapshot->header.h, extent);
        std::vector<lv_color32_t> pixels;
        pixels.reserve(extent * extent);
        for(int row = 0; row < extent; ++row) {
            auto* source = reinterpret_cast<const lv_color32_t*>(snapshot->data + row * snapshot->header.stride);
            pixels.insert(pixels.end(), source, source + extent);
        }
        return pixels;
    }
};

void CheckPixel(const std::vector<lv_color32_t>& pixels, int column, int row, int red, int green) {
    const auto& pixel = pixels[row * extent + column];
    zassert_within(pixel.red, red, 8, "red at %d,%d: %d != %d", column, row, pixel.red, red);
    zassert_within(pixel.green, green, 8, "green at %d,%d: %d != %d", column, row, pixel.green, green);
    zassert_equal(pixel.blue, 0);
}

int32_t DrawMargin(lv_obj_t* object) {
    int32_t margin = 0;
    lv_obj_send_event(object, LV_EVENT_REFR_EXT_DRAW_SIZE, &margin);
    return margin;
}

void* Setup() {
    views_test::EnsureTestDisplay();
    eerie_leap::event_bus::InitializeEventChannels();
    return nullptr;
}

} // namespace

extern "C" lv_anim_t* __real_lv_anim_start(const lv_anim_t* animation);

extern "C" lv_anim_t* __wrap_lv_anim_start(const lv_anim_t* animation) {
    ++animation_start_calls;
    if(std::exchange(fail_next_animation_start, false))
        return nullptr;
    return __real_lv_anim_start(animation);
}

ZTEST_SUITE(widget_animations, NULL, Setup, NULL, views_test::CleanTestDisplay, NULL);

ZTEST(widget_animations, test_widget_animation_adapter_stages_properties_until_synchronization) {
    ScopedLvglLock lock;
    const auto count = lv_anim_count_running();
    const auto attempts = animation_start_calls;
    Frame layout = Frame::CreateWrapped().Build();
    Frame content = Frame::CreatePresentation(layout.GetObject()).Build();
    const auto callbacks = lv_obj_get_event_count(content.GetObject());
    WidgetPropertyStore store;
    WidgetAnimation::RegisterProperties(store);
    zassert_equal(store.GetRegisteredTypes().size(), 3);
    zassert_equal(store.GetAs<int>(WidgetPropertyType::ANIMATION_TYPE, -1), 0);
    zassert_false(store.GetAs<bool>(WidgetPropertyType::IS_ANIMATION_ACTIVE, true));
    zassert_equal(store.GetAs<int>(WidgetPropertyType::ANIMATION_DURATION_MS, -1), 1000);
    WidgetAnimation animation;
    bool eligible = true;
    zassert_true(animation.Attach(content, layout, [](void* context) {
        return *static_cast<bool*>(context);
    }, &eligible));
    for(auto type : store.GetRegisteredTypes()) {
        zassert_equal(store.GetEffect(type), PropertyChangeEffect::None);
        zassert_true(animation.ApplyProperty(type, store.Get(type)));
    }
    zassert_false(animation.ApplyProperty(WidgetPropertyType::VALUE, 42.0));
    animation.Synchronize();
    zassert_equal(animation_start_calls, attempts);
    zassert_true(animation.ApplyProperty(WidgetPropertyType::ANIMATION_TYPE, 2));
    zassert_true(animation.ApplyProperty(WidgetPropertyType::IS_ANIMATION_ACTIVE, true));
    zassert_true(animation.ApplyProperty(WidgetPropertyType::ANIMATION_DURATION_MS, 2000));
    zassert_equal(animation_start_calls, attempts);
    animation.Synchronize();
    zassert_equal(animation_start_calls, attempts + 1);
    Advance(500);
    zassert_equal(lv_obj_get_style_transform_rotation(content.GetObject(), LV_PART_MAIN), 900);
    animation.Synchronize();
    zassert_equal(animation_start_calls, attempts + 1);
    animation.SetOwner(false);
    Advance(250);
    zassert_equal(lv_anim_count_running(), count);
    zassert_equal(lv_obj_get_style_transform_rotation(content.GetObject(), LV_PART_MAIN), 0);
    animation.SetOwner(true);
    animation.Synchronize();
    zassert_equal(animation_start_calls, attempts + 2);
    eligible = false;
    Advance(250);
    zassert_equal(lv_anim_count_running(), count);
    eligible = true;
    animation.Synchronize();
    zassert_equal(animation_start_calls, attempts + 3);
    animation.Detach();
    animation.Detach();
    animation.StopAndReset();
    animation.Synchronize();
    zassert_equal(lv_anim_count_running(), count);
    zassert_equal(animation_start_calls, attempts + 3);
    zassert_equal(lv_obj_get_event_count(content.GetObject()), callbacks);
}

ZTEST(widget_animations, test_widget_animation_render_order_failures_and_recovery) {
    ScopedLvglLock lock;
    const auto count = lv_anim_count_running();
    std::array<int, 3> order { 0, 1, 2 };
    do {
        auto root = std::make_shared<Frame>(Frame::CreateWrapped().Build());
        LifecycleProbe widget(1, root, WidgetContext{});
        auto configuration = AnimationConfiguration();
        const auto attempts = animation_start_calls;
        for(auto step : order) {
            if(step == 0)
                widget.Configure(configuration);
            else if(step == 1)
                zassert_equal(widget.Render(), 0);
            else
                widget.OnActivated();
        }
        zassert_equal(animation_start_calls, attempts + 1);
        zassert_equal(lv_anim_count_running(), count + 1);
        auto* content = views_test::WidgetContent(widget);
        Advance(250);
        zassert_equal(lv_obj_get_style_transform_rotation(content, LV_PART_MAIN), 900);
        widget.render_result = -1;
        zassert_equal(widget.Render(), -1);
        zassert_false(widget.IsReady());
        zassert_equal(lv_anim_count_running(), count);
        zassert_equal(lv_obj_get_style_transform_rotation(content, LV_PART_MAIN), 0);
        Publish(WidgetPropertyType::ANIMATION_DURATION_MS, 2000);
        widget.render_result = 0;
        widget.theme_result = -2;
        zassert_equal(widget.Render(), -2);
        Refresh();
        zassert_false(widget.IsReady());
        zassert_equal(animation_start_calls, attempts + 1);
        widget.theme_result = 0;
        zassert_equal(widget.Render(), 0);
        zassert_equal(animation_start_calls, attempts + 2);
        zassert_equal(views_test::WidgetContent(widget), content);
        Advance(500);
        zassert_equal(lv_obj_get_style_transform_rotation(content, LV_PART_MAIN), 900);
    } while(std::next_permutation(order.begin(), order.end()));
    zassert_equal(lv_anim_count_running(), count);
}

ZTEST(widget_animations, test_widget_animation_runtime_settings_are_base_owned_and_preserve_phase) {
    ScopedLvglLock lock;
    const auto count = lv_anim_count_running();
    auto root = std::make_shared<Frame>(Frame::CreateWrapped().Build());
    LifecycleProbe widget(1, root, WidgetContext{});
    widget.Configure(AnimationConfiguration());
    zassert_equal(widget.Render(), 0);
    widget.OnActivated();
    widget.notified.clear();
    auto* content = views_test::WidgetContent(widget);
    const auto attempts = animation_start_calls;
    Advance(250);
    for(int iteration = 0; iteration < 4; ++iteration) {
        Publish(WidgetPropertyType::ANIMATION_TYPE, 2);
        Publish(WidgetPropertyType::IS_ANIMATION_ACTIVE, true);
        Publish(WidgetPropertyType::ANIMATION_DURATION_MS, 1000);
        Refresh();
    }
    for(auto invalid : { -1, 3 })
        Publish(WidgetPropertyType::ANIMATION_TYPE, invalid);
    Publish(WidgetPropertyType::ANIMATION_DURATION_MS, 1.5F);
    Publish(WidgetPropertyType::IS_ANIMATION_ACTIVE, 2);
    zassert_equal(animation_start_calls, attempts);
    zassert_equal(lv_obj_get_style_transform_rotation(content, LV_PART_MAIN), 900);
    zassert_true(widget.notified.empty());
    Publish(WidgetPropertyType::ANIMATION_DURATION_MS, 2000);
    zassert_equal(animation_start_calls, attempts + 1);
    zassert_equal(lv_obj_get_style_transform_rotation(content, LV_PART_MAIN), 0);
    Publish(WidgetPropertyType::ANIMATION_TYPE, 1);
    zassert_equal(animation_start_calls, attempts + 2);
    Advance(1000);
    zassert_equal(lv_obj_get_style_opa_layered(content, LV_PART_MAIN), 0);
    zassert_true(widget.IsProcessingEligible());
    Publish(WidgetPropertyType::VALUE, 42.0F);
    zassert_equal(widget.applied_value, 42.0);
    zassert_equal(animation_start_calls, attempts + 2);
    Publish(WidgetPropertyType::IS_ANIMATION_ACTIVE, false);
    zassert_equal(lv_anim_count_running(), count);
    zassert_equal(lv_obj_get_style_opa_layered(content, LV_PART_MAIN), 255);
    Publish(WidgetPropertyType::IS_ANIMATION_ACTIVE, true);
    zassert_equal(animation_start_calls, attempts + 3);
    Publish(WidgetPropertyType::ANIMATION_TYPE, 0);
    zassert_equal(lv_anim_count_running(), count);
    widget.Detach();
    widget.Detach();
    Publish(WidgetPropertyType::ANIMATION_TYPE, 2);
    Refresh();
    zassert_equal(animation_start_calls, attempts + 3);
}

ZTEST(widget_animations, test_widget_animation_restoration_batches_settings_and_value_after_all_gates_open) {
    ScopedLvglLock lock;
    const auto count = lv_anim_count_running();
    for(int gate = 0; gate < 8; ++gate) {
        auto root = std::make_shared<Frame>(Frame::CreateWrapped().Build());
        LifecycleProbe widget(1, root, WidgetContext{});
        widget.Configure(AnimationConfiguration());
        zassert_equal(widget.Render(), 0);
        widget.OnActivated();
        auto set_gate = [&](bool enabled) {
            switch(gate) {
                case 0: Publish(WidgetPropertyType::IS_VISIBLE, enabled); break;
                case 1: Publish(WidgetPropertyType::OPACITY, enabled ? 255 : 0); break;
                case 2: if(enabled) widget.OnActivated(); else widget.OnDeactivated(); break;
                case 3: lv_obj_set_flag(root->GetObject(), LV_OBJ_FLAG_HIDDEN, !enabled); break;
                case 4: lv_obj_set_style_opa(root->GetObject(), enabled ? 255 : 0, LV_PART_MAIN); break;
                case 5: lv_obj_set_style_opa_layered(root->GetObject(), enabled ? 255 : 0, LV_PART_MAIN); break;
                case 6: root->SetProcessingEnabled(enabled); break;
                case 7: Publish(WidgetPropertyType::IS_ACTIVE, enabled); break;
            }
        };
        Advance(250);
        set_gate(false);
        Advance(1);
        zassert_equal(lv_anim_count_running(), count);
        Publish(WidgetPropertyType::IS_VISIBLE, false);
        Publish(WidgetPropertyType::ANIMATION_TYPE, 1);
        Publish(WidgetPropertyType::ANIMATION_DURATION_MS, 2000);
        Publish(WidgetPropertyType::VALUE, 81.0F);
        Publish(WidgetPropertyType::VALUE, 82.0F);
        const auto attempts = animation_start_calls;
        Advance(9000);
        set_gate(true);
        Refresh();
        if(gate != 0) {
            zassert_equal(lv_anim_count_running(), count);
            zassert_equal(animation_start_calls, attempts);
            Publish(WidgetPropertyType::IS_VISIBLE, true);
        }
        zassert_equal(animation_start_calls, attempts + 1);
        zassert_equal(lv_anim_count_running(), count + 1);
        if(gate != 7) {
            zassert_equal(widget.applied_value, 82.0);
            zassert_equal(widget.starts_at_value, attempts);
        }
        auto* content = views_test::WidgetContent(widget);
        zassert_equal(lv_obj_get_style_transform_rotation(content, LV_PART_MAIN), 0);
        zassert_equal(lv_obj_get_style_opa_layered(content, LV_PART_MAIN), 255);
        Advance(gate == 7 ? 250 : 1000);
        if(gate == 7)
            zassert_equal(lv_obj_get_style_transform_rotation(content, LV_PART_MAIN), 900);
        else
            zassert_equal(lv_obj_get_style_opa_layered(content, LV_PART_MAIN), 0);
        Refresh();
        zassert_equal(animation_start_calls, attempts + 1);
    }
    zassert_equal(lv_anim_count_running(), count);
}

ZTEST(widget_animations, test_animator_start_failure_stays_neutral_and_retries_only_on_explicit_transitions) {
    ScopedLvglLock lock;
    for(auto type : { Animation::Type::Blinking, Animation::Type::Rotation }) {
        for(int transition : { 0, 1, 2 }) {
            AnimatorScene scene;
            const auto count = lv_anim_count_running();
            const auto attempts = animation_start_calls;
            ViewAnimator::Settings settings { type, true, 1000 };
            fail_next_animation_start = true;
            scene.animator.Synchronize(settings);
            zassert_equal(animation_start_calls, attempts + 1);
            zassert_false(scene.animator.IsRunning());
            zassert_equal(lv_anim_count_running(), count);
            zassert_equal(scene.Opacity(), 255);
            zassert_equal(scene.Angle(), 0);
            zassert_false(lv_obj_has_flag(scene.layout.GetObject(), LV_OBJ_FLAG_OVERFLOW_VISIBLE));
            zassert_equal(DrawMargin(scene.layout.GetObject()), 0);
            for(int iteration = 0; iteration < 5; ++iteration) {
                Advance(100);
                scene.animator.Synchronize(settings);
            }
            zassert_equal(animation_start_calls, attempts + 1);
            if(transition == 0)
                settings.duration_ms = 2000;
            else if(transition == 1) {
                scene.eligible = false;
                scene.animator.Synchronize(settings);
                scene.eligible = true;
            } else {
                zassert_true(scene.animator.Attach(scene.presentation, scene.layout,
                    [](void* context) { return *static_cast<bool*>(context); }, &scene.eligible));
            }
            scene.animator.Synchronize(settings);
            zassert_equal(animation_start_calls, attempts + 2);
            zassert_true(scene.animator.IsRunning());
            zassert_equal(lv_anim_count_running(), count + 1);
        }
    }
}

ZTEST(widget_animations, test_animator_defaults_and_stop_are_neutral_without_registrations) {
    ScopedLvglLock lock;
    const auto count = lv_anim_count_running();
    {
        AnimatorScene scene;
        for(auto settings : { ViewAnimator::Settings{}, ViewAnimator::Settings{ Animation::Type::None, true, 1000 },
                             ViewAnimator::Settings{ Animation::Type::Blinking, false, 1000 } }) {
            scene.animator.Synchronize(settings);
            zassert_false(scene.animator.IsRunning());
            zassert_equal(scene.Opacity(), 255);
            zassert_equal(scene.Angle(), 0);
            zassert_equal(lv_anim_count_running(), count);
        }
        scene.animator.StopAndReset();
        scene.animator.StopAndReset();
    }
    zassert_equal(lv_anim_count_running(), count);
}

ZTEST(widget_animations, test_animator_rejects_invalid_settings_without_changing_running_phase) {
    ScopedLvglLock lock;
    AnimatorScene scene;
    scene.animator.Synchronize({ Animation::Type::Rotation, true, 1000 });
    Advance(250);
    const auto attempts = animation_start_calls;
    for(auto settings : { ViewAnimator::Settings{ Animation::Type::Rotation, true, -1 },
                         ViewAnimator::Settings{ Animation::Type::Rotation, true, 0 },
                         ViewAnimator::Settings{ Animation::Type::Rotation, true, 1 },
                         ViewAnimator::Settings{ static_cast<Animation::Type>(3), true, 1000 } }) {
        scene.animator.Synchronize(settings);
        zassert_true(scene.animator.IsRunning());
        zassert_equal(scene.Angle(), 900);
        zassert_equal(animation_start_calls, attempts);
    }
    scene.animator.Synchronize({ Animation::Type::Rotation, true, Animation::MAX_DURATION_MS });
    zassert_equal(lv_anim_get(&scene.animator, nullptr)->duration, Animation::MAX_DURATION_MS);
    zassert_equal(scene.Angle(), 0);
    scene.animator.Synchronize({ Animation::Type::Blinking, true, Animation::MAX_DURATION_MS });
    auto* registration = lv_anim_get(&scene.animator, nullptr);
    zassert_equal(registration->duration, Animation::MAX_DURATION_MS / 2);
    zassert_equal(registration->reverse_duration, Animation::MAX_DURATION_MS - Animation::MAX_DURATION_MS / 2);
    zassert_equal(registration->path_cb, lv_anim_path_ease_in_out);
}

ZTEST(widget_animations, test_animator_blink_uses_two_halves_of_one_even_or_odd_cycle) {
    ScopedLvglLock lock;
    for(int duration : { 1000, 1001, 2, 3 }) {
        AnimatorScene scene;
        scene.animator.Synchronize({ Animation::Type::Blinking, true, duration });
        zassert_true(scene.animator.IsRunning());
        zassert_equal(scene.Opacity(), 255);
        const int forward = duration / 2;
        const int reverse = duration - forward;
        if(forward > 1) {
            Advance(forward / 2);
            zassert_true(scene.Opacity() > 0 && scene.Opacity() < 255);
        }
        Advance(forward - (forward > 1 ? forward / 2 : 0));
        zassert_equal(scene.Opacity(), 0);
        zassert_true(scene.animator.IsRunning());
        if(reverse > 1) {
            Advance(reverse / 2);
            zassert_true(scene.Opacity() > 0 && scene.Opacity() < 255);
        }
        Advance(reverse - (reverse > 1 ? reverse / 2 : 0));
        zassert_equal(scene.Opacity(), 255);
        zassert_true(scene.animator.IsRunning());
        Advance(forward);
        zassert_equal(scene.Opacity(), 0);
        scene.animator.StopAndReset();
        zassert_equal(scene.Opacity(), 255);
        zassert_is_null(lv_anim_get(&scene.animator, nullptr));
    }
}

ZTEST(widget_animations, test_animator_rotation_is_linear_clockwise_and_repeats_without_drift) {
    ScopedLvglLock lock;
    AnimatorScene scene;
    scene.animator.Synchronize({ Animation::Type::Rotation, true, 1000 });
    zassert_equal(scene.Angle(), 0);
    for(int cycle = 0; cycle < 3; ++cycle) {
        for(int quarter = 1; quarter <= 4; ++quarter) {
            Advance(250);
            zassert_equal(scene.Angle(), quarter * 900);
            zassert_true(scene.animator.IsRunning());
        }
    }
    scene.animator.StopAndReset();
    zassert_equal(scene.Angle(), 0);
    zassert_equal(scene.Opacity(), 255);
}

ZTEST(widget_animations, test_animator_same_settings_preserve_phase_and_changed_settings_restart_once) {
    ScopedLvglLock lock;
    const auto count = lv_anim_count_running();
    AnimatorScene scene;
    ViewAnimator::Settings settings { Animation::Type::Rotation, true, 1000 };
    scene.animator.Synchronize(settings);
    Advance(250);
    auto* registration = lv_anim_get(&scene.animator, nullptr);
    const auto attempts = animation_start_calls;
    for(int iteration = 0; iteration < 10; ++iteration) {
        scene.animator.Synchronize(settings);
        lv_refr_now(nullptr);
        zassert_equal(lv_anim_get(&scene.animator, nullptr), registration);
        zassert_equal(registration->act_time, 250);
        zassert_equal(scene.Angle(), 900);
        zassert_equal(lv_anim_count_running(), count + 1);
    }
    zassert_equal(animation_start_calls, attempts);
    settings.duration_ms = 2000;
    scene.animator.Synchronize(settings);
    zassert_equal(animation_start_calls, attempts + 1);
    zassert_equal(scene.Angle(), 0);
    Advance(500);
    zassert_equal(scene.Angle(), 900);
    settings.type = Animation::Type::Blinking;
    scene.animator.Synchronize(settings);
    zassert_equal(animation_start_calls, attempts + 2);
    zassert_equal(scene.Angle(), 0);
    zassert_equal(scene.Opacity(), 255);
    zassert_false(lv_obj_has_flag(scene.layout.GetObject(), LV_OBJ_FLAG_OVERFLOW_VISIBLE));
    Advance(1000);
    zassert_equal(scene.Opacity(), 0);
    settings.active = false;
    scene.animator.Synchronize(settings);
    zassert_false(scene.animator.IsRunning());
    zassert_equal(scene.Opacity(), 255);
    zassert_equal(lv_anim_count_running(), count);
    settings.active = true;
    scene.animator.Synchronize(settings);
    zassert_equal(scene.Opacity(), 255);
    zassert_equal(lv_anim_count_running(), count + 1);
    settings.type = Animation::Type::None;
    scene.animator.Synchronize(settings);
    zassert_false(scene.animator.IsRunning());
    zassert_equal(lv_anim_count_running(), count);
}

ZTEST(widget_animations, test_animator_checks_eligibility_at_each_execution_and_restarts_without_catchup) {
    ScopedLvglLock lock;
    for(auto type : { Animation::Type::Blinking, Animation::Type::Rotation }) {
        AnimatorScene scene;
        const ViewAnimator::Settings settings { type, true, 1000 };
        scene.eligible = false;
        scene.animator.Synchronize(settings);
        zassert_false(scene.animator.IsRunning());
        scene.eligible = true;
        scene.animator.Synchronize(settings);
        Advance(250);
        scene.eligible = false;
        Advance(250);
        zassert_false(scene.animator.IsRunning());
        zassert_equal(scene.Opacity(), 255);
        zassert_equal(scene.Angle(), 0);
        zassert_is_null(lv_anim_get(&scene.animator, nullptr));
        scene.animator.Synchronize(settings);
        Advance(9000);
        scene.eligible = true;
        scene.animator.Synchronize(settings);
        zassert_true(scene.animator.IsRunning());
        zassert_equal(scene.Opacity(), 255);
        zassert_equal(scene.Angle(), 0);
        Advance(250);
        if(type == Animation::Type::Rotation)
            zassert_equal(scene.Angle(), 900);
        else
            zassert_true(scene.Opacity() > 0 && scene.Opacity() < 255);
    }
}

ZTEST(widget_animations, test_animator_synchronous_first_callback_can_suspend_without_deleting_during_start) {
    ScopedLvglLock lock;
    AnimatorScene scene;
    struct Eligibility {
        int calls = 0;
        bool reject_start = true;
    } eligibility;
    zassert_true(scene.animator.Attach(scene.presentation, scene.layout, [](void* context) {
        auto* state = static_cast<Eligibility*>(context);
        return ++state->calls != 2 || !state->reject_start;
    }, &eligibility));
    scene.animator.Synchronize({ Animation::Type::Blinking, true, 1000 });
    zassert_equal(eligibility.calls, 2);
    zassert_false(scene.animator.IsRunning());
    zassert_equal(scene.Opacity(), 255);
    zassert_is_null(lv_anim_get(&scene.animator, nullptr));
    eligibility.reject_start = false;
    scene.animator.Synchronize({ Animation::Type::Blinking, true, 1000 });
    zassert_true(scene.animator.IsRunning());
    Advance(500);
    zassert_equal(scene.Opacity(), 0);
}

ZTEST(widget_animations, test_animator_cancellation_uses_exact_executor_and_deleted_callback_only_clears_state) {
    ScopedLvglLock lock;
    const auto count = lv_anim_count_running();
    AnimatorScene scene;
    lv_anim_t other;
    lv_anim_init(&other);
    lv_anim_set_exec_cb(&other, OtherAnimation);
    lv_anim_set_repeat_count(&other, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_var(&other, &scene.animator);
    auto* same_variable = lv_anim_start(&other);
    lv_anim_set_var(&other, scene.presentation.GetObject());
    auto* same_target = lv_anim_start(&other);
    const ViewAnimator::Settings settings { Animation::Type::Blinking, true, 1000 };
    scene.animator.Synchronize(settings);
    Advance(250);
    auto* owned = lv_anim_get(&scene.animator, nullptr);
    zassert_not_equal(owned->exec_cb, OtherAnimation);
    const auto opacity = scene.Opacity();
    zassert_true(lv_anim_delete(&scene.animator, owned->exec_cb));
    zassert_false(scene.animator.IsRunning());
    zassert_equal(scene.Opacity(), opacity);
    scene.animator.Synchronize(settings);
    zassert_false(scene.animator.IsRunning());
    scene.animator.StopAndReset();
    scene.animator.Synchronize(settings);
    scene.animator.StopAndReset();
    scene.animator.Detach();
    scene.animator.Detach();
    zassert_equal(lv_anim_get(&scene.animator, OtherAnimation), same_variable);
    zassert_equal(lv_anim_get(scene.presentation.GetObject(), OtherAnimation), same_target);
    zassert_equal(lv_anim_count_running(), count + 2);
    lv_anim_delete(&scene.animator, OtherAnimation);
    lv_anim_delete(scene.presentation.GetObject(), OtherAnimation);
    zassert_equal(lv_anim_count_running(), count);
}

ZTEST(widget_animations, test_animator_live_reattachment_resets_old_target_without_affecting_another_helper) {
    ScopedLvglLock lock;
    const auto count = lv_anim_count_running();
    AnimatorScene first;
    AnimatorScene second;
    Frame replacement = Frame::CreatePresentation(first.layout.GetObject()).Build();
    const ViewAnimator::Settings fade { Animation::Type::Blinking, true, 1000 };
    first.animator.Synchronize(fade);
    second.animator.Synchronize({ Animation::Type::Rotation, true, 1000 });
    Advance(250);
    zassert_equal(lv_anim_count_running(), count + 2);
    Frame empty = Frame::Create(nullptr);
    zassert_false(first.animator.Attach(empty, first.layout, [](void*) { return true; }, nullptr));
    zassert_false(first.animator.Attach(first.presentation, empty, [](void*) { return true; }, nullptr));
    zassert_false(first.animator.Attach(first.presentation, first.layout, nullptr, nullptr));
    zassert_false(first.animator.Attach(first.presentation, second.layout,
        [](void*) { return true; }, nullptr));
    zassert_true(first.animator.IsRunning());
    const auto callbacks = lv_obj_get_event_count(first.layout.GetObject());
    zassert_true(first.animator.Attach(replacement, first.layout,
        [](void*) { return true; }, nullptr));
    zassert_equal(first.Opacity(), 255);
    zassert_false(first.animator.IsRunning());
    zassert_equal(lv_obj_get_event_count(first.layout.GetObject()), callbacks);
    zassert_equal(lv_anim_count_running(), count + 1);
    first.animator.Synchronize(fade);
    zassert_equal(second.Angle(), 900);
    Advance(250);
    zassert_equal(second.Angle(), 1800);
    zassert_equal(first.Opacity(), 255);
    const auto opacity = lv_obj_get_style_opa_layered(replacement.GetObject(), LV_PART_MAIN);
    zassert_true(opacity > 0 && opacity < 255);
    first.animator.Detach();
    zassert_true(second.animator.IsRunning());
    zassert_equal(lv_anim_count_running(), count + 1);
}

ZTEST(widget_animations, test_animator_target_and_wrapper_deletion_cancel_and_allow_reattachment) {
    ScopedLvglLock lock;
    const auto count = lv_anim_count_running();
    for(auto type : { Animation::Type::Blinking, Animation::Type::Rotation }) {
        for(bool delete_wrapper : { false, true }) {
            ViewAnimator animator;
            auto layout = std::make_unique<Frame>(Frame::CreateWrapped().SetWidth(64, true).SetHeight(24, true).Build());
            auto target = std::make_unique<Frame>(Frame::CreatePresentation(layout->GetObject()).Build());
            const auto callbacks = lv_obj_get_event_count(layout->GetObject());
            zassert_true(animator.Attach(*target, *layout, [](void*) { return true; }, nullptr));
            animator.Synchronize({ type, true, 1000 });
            Advance(500);
            if(delete_wrapper) {
                lv_obj_add_event_cb(layout->GetObject(), [](lv_event_t* event) {
                    static_cast<std::unique_ptr<Frame>*>(lv_event_get_user_data(event))->reset();
                }, LV_EVENT_DELETE, &target);
                layout.reset();
                zassert_is_null(target.get());
            } else {
                target.reset();
                zassert_equal(lv_obj_get_event_count(layout->GetObject()), callbacks);
                zassert_false(lv_obj_has_flag(layout->GetObject(), LV_OBJ_FLAG_OVERFLOW_VISIBLE));
                zassert_equal(DrawMargin(layout->GetObject()), 0);
            }
            zassert_false(animator.IsRunning());
            zassert_equal(lv_anim_count_running(), count);
            Advance(1000);
            animator.Synchronize({ type, true, 1000 });
            zassert_false(animator.IsRunning());
            Frame replacement = Frame::CreateWrapped().Build();
            Frame content = Frame::CreatePresentation(replacement.GetObject()).Build();
            zassert_true(animator.Attach(content, replacement, [](void*) { return true; }, nullptr));
            animator.Synchronize({ type, true, 1000 });
            zassert_true(animator.IsRunning());
            animator.Detach();
            zassert_equal(lv_anim_count_running(), count);
        }
    }
}

ZTEST(widget_animations, test_animator_scope_exit_restores_flags_margins_and_callbacks_for_both_effects) {
    ScopedLvglLock lock;
    const auto count = lv_anim_count_running();
    for(auto type : { Animation::Type::Blinking, Animation::Type::Rotation }) {
        for(int elapsed : { 250, 500 }) {
            for(bool overflow : { false, true }) {
                AnimatorScene scene;
                scene.animator.Detach();
                auto* layout = scene.layout.GetObject();
                auto* content = scene.presentation.GetObject();
                lv_obj_set_flag(layout, LV_OBJ_FLAG_OVERFLOW_VISIBLE, overflow);
                lv_obj_set_flag(content, LV_OBJ_FLAG_OVERFLOW_VISIBLE, !overflow);
                lv_obj_set_ext_draw_size(layout, 3);
                lv_obj_refresh_ext_draw_size(layout);
                const auto layout_callbacks = lv_obj_get_event_count(layout);
                const auto content_callbacks = lv_obj_get_event_count(content);
                {
                    ViewAnimator animator;
                    zassert_true(animator.Attach(scene.presentation, scene.layout, [](void*) { return true; }, nullptr));
                    animator.Synchronize({ type, true, 1000 });
                    Advance(elapsed);
                    zassert_equal(lv_anim_count_running(), count + 1);
                }
                zassert_equal(lv_anim_count_running(), count);
                zassert_equal(scene.Opacity(), 255);
                zassert_equal(scene.Angle(), 0);
                zassert_equal(DrawMargin(layout), 3);
                zassert_equal(lv_obj_has_flag(layout, LV_OBJ_FLAG_OVERFLOW_VISIBLE), overflow);
                zassert_equal(lv_obj_has_flag(content, LV_OBJ_FLAG_OVERFLOW_VISIBLE), !overflow);
                zassert_equal(lv_obj_get_event_count(layout), layout_callbacks);
                zassert_equal(lv_obj_get_event_count(content), content_callbacks);
            }
        }
    }
}

ZTEST(widget_animations, test_animator_rotation_pixels_resize_and_external_clipping_match_feasibility) {
    ScopedLvglLock lock;
    Scene scene;
    const auto baseline = scene.Pixels();
    ViewAnimator animator;
    zassert_true(animator.Attach(*scene.widget.Content(), *scene.widget.GetContainer(),
        [](void*) { return true; }, nullptr));
    const ViewAnimator::Settings settings { Animation::Type::Rotation, true, 1000 };
    animator.Synchronize(settings);
    Advance(125);
    auto pixels = scene.Pixels();
    CheckPixel(pixels, 34, 23, 255, 0);
    CheckPixel(pixels, 59, 68, 0, 255);
    Advance(125);
    pixels = scene.Pixels();
    CheckPixel(pixels, 56, 20, 255, 0);
    CheckPixel(pixels, 42, 70, 0, 255);
    scene.clip->SetHeight(48, true);
    pixels = scene.Pixels();
    CheckPixel(pixels, 56, 20, 255, 0);
    CheckPixel(pixels, 42, 70, 0, 0);
    scene.clip->SetHeight(extent, true);
    scene.widget.SetPositionPx({ 12, 32 });
    scene.widget.SetSizePx({ 72, 32 });
    pixels = scene.Pixels();
    CheckPixel(pixels, 60, 16, 255, 0);
    CheckPixel(pixels, 48, 58, 0, 255);
    zassert_equal(lv_anim_get(&animator, nullptr)->act_time, 250);
    scene.widget.SetPositionPx({ 16, 36 });
    scene.widget.SetSizePx({ 64, 24 });
    animator.StopAndReset();
    const auto restored = scene.Pixels();
    zassert_mem_equal(baseline.data(), restored.data(), baseline.size() * sizeof(lv_color32_t));
}

ZTEST(widget_animations, test_animator_fade_renders_full_mid_zero_and_return_without_suspending_data) {
    ScopedLvglLock lock;
    Scene scene;
    lv_obj_set_style_opa_layered(scene.widget.GetContainer()->GetObject(), 128, LV_PART_MAIN);
    auto* presentation = views_test::WidgetContent(scene.widget);
    auto* leaf = lv_obj_get_child(presentation, 0);
    lv_obj_set_style_bg_opa(leaf, 128, LV_PART_MAIN);
    ViewAnimator animator;
    zassert_true(animator.Attach(*scene.widget.Content(), *scene.widget.GetContainer(),
        [](void*) { return true; }, nullptr));
    animator.Synchronize({ Animation::Type::Blinking, true, 1000 });
    CheckPixel(scene.Pixels(), 20, 40, 64, 0);
    Advance(250);
    CheckPixel(scene.Pixels(), 20, 40, 32, 0);
    Advance(250);
    CheckPixel(scene.Pixels(), 20, 40, 0, 0);
    zassert_true(animator.IsRunning());
    zassert_true(scene.widget.IsProcessingEligible());
    SensorEventsChannel::GetInstance().Publish({
        .source_id = 0,
        .type = SensorEventType::DataUpdated,
        .payload = {{ SensorPayloadType::Value, 62.0F }}
    });
    zassert_equal(scene.widget.applied_value, 62.0);
    Advance(250);
    CheckPixel(scene.Pixels(), 20, 40, 32, 0);
    Advance(250);
    CheckPixel(scene.Pixels(), 20, 40, 64, 0);
    animator.StopAndReset();
    zassert_equal(lv_obj_get_style_bg_opa(leaf, LV_PART_MAIN), 128);
    zassert_equal(lv_obj_get_style_opa_layered(scene.widget.GetContainer()->GetObject(), LV_PART_MAIN), 128);
}

ZTEST(widget_animations, test_presentation_frame_is_neutral_stable_and_not_an_input_surface) {
    ScopedLvglLock lock;
    Scene scene;
    auto* layout = scene.widget.GetContainer()->GetObject();
    auto* content = views_test::WidgetContent(scene.widget);
    zassert_equal(lv_obj_get_parent(content), layout);
    zassert_equal(lv_obj_get_width(content), 64);
    zassert_equal(lv_obj_get_height(content), 24);
    zassert_equal(lv_obj_get_style_bg_opa(content, LV_PART_MAIN), LV_OPA_TRANSP);
    zassert_equal(lv_obj_get_style_border_width(content, LV_PART_MAIN), 0);
    zassert_equal(lv_obj_get_style_pad_left(content, LV_PART_MAIN), 0);
    zassert_equal(lv_obj_get_style_pad_right(content, LV_PART_MAIN), 0);
    zassert_equal(lv_obj_get_style_pad_top(content, LV_PART_MAIN), 0);
    zassert_equal(lv_obj_get_style_pad_bottom(content, LV_PART_MAIN), 0);
    zassert_equal(lv_obj_get_style_opa_layered(content, LV_PART_MAIN), LV_OPA_COVER);
    zassert_equal(lv_obj_get_style_transform_rotation(content, LV_PART_MAIN), 0);
    for(auto flag : { LV_OBJ_FLAG_CLICKABLE, LV_OBJ_FLAG_CLICK_FOCUSABLE, LV_OBJ_FLAG_SCROLLABLE,
                     LV_OBJ_FLAG_SCROLL_ON_FOCUS })
        zassert_false(lv_obj_has_flag(content, flag));
    zassert_true(lv_obj_has_flag(content, LV_OBJ_FLAG_GESTURE_BUBBLE));
    zassert_is_null(lv_obj_get_user_data(content));
    scene.widget.Configure(Configuration());
    scene.widget.OnDeactivated();
    scene.widget.OnActivated();
    zassert_equal(views_test::WidgetContent(scene.widget), content);
    zassert_is_null(lv_anim_get(&scene.widget, nullptr));
}

ZTEST(widget_animations, test_presentation_opacity_preserves_widget_icon_and_descendant_processing) {
    ScopedLvglLock lock;
    Scene scene;
    PresentationProbe descendant(2, scene.widget.Content(), WidgetContext{});
    descendant.Configure(Configuration());
    zassert_equal(descendant.Render(), 0);
    descendant.OnActivated();
    auto properties = std::make_shared<WidgetPropertyStore>();
    TestOval::RegisterProperties(*properties);
    TestOval oval(scene.widget.Content());
    oval.Configure(properties);
    zassert_equal(oval.Render(), 0);
    oval.SetProcessingEnabled(true);
    auto check = [&](bool eligible) {
        zassert_equal(scene.widget.IsProcessingEligible(), eligible);
        zassert_equal(descendant.IsProcessingEligible(), eligible);
        zassert_equal(oval.IsProcessingEligible(), eligible);
    };
    auto* presentation = views_test::WidgetContent(scene.widget);
    lv_obj_set_style_opa_layered(presentation, 0, 0);
    lv_refr_now(nullptr);
    check(true);
    zassert_true(scene.widget.IsTrackingEligible());
    SensorEventsChannel::GetInstance().Publish({
        .source_id = 0,
        .type = SensorEventType::DataUpdated,
        .payload = {{ SensorPayloadType::Value, 42.0F }}
    });
    zassert_equal(scene.widget.applied_value, 42.0);
    zassert_equal(descendant.applied_value, 42.0);

    for(auto* target : { presentation, scene.widget.GetContainer()->GetObject(), scene.root->GetObject() }) {
        lv_obj_add_flag(target, LV_OBJ_FLAG_HIDDEN);
        check(false);
        lv_obj_remove_flag(target, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(target, 0, 0);
        check(false);
        lv_obj_set_style_opa(target, 255, 0);
        check(true);
        if(target != presentation) {
            lv_obj_set_style_opa_layered(target, 0, 0);
            check(false);
            lv_obj_set_style_opa_layered(target, 255, 0);
            check(true);
        }
    }
    scene.widget.OnDeactivated();
    check(false);
    scene.widget.OnActivated();
    check(true);
}

ZTEST(widget_animations, test_asymmetric_subtree_rotates_without_internal_clipping_and_resets) {
    ScopedLvglLock lock;
    Scene scene;
    auto* presentation = views_test::WidgetContent(scene.widget);
    const auto baseline = scene.Pixels();
    CheckPixel(baseline, 20, 40, 255, 0);
    CheckPixel(baseline, 70, 54, 0, 255);
    const auto count = lv_anim_count_running();
    PresentationStyles styles(scene.widget);
    styles.Rotate(900);
    auto rotated = scene.Pixels();
    CheckPixel(rotated, 56, 20, 255, 0);
    CheckPixel(rotated, 42, 70, 0, 255);
    CheckPixel(rotated, 20, 40, 0, 0);
    styles.Rotate(450);
    rotated = scene.Pixels();
    CheckPixel(rotated, 34, 23, 255, 0);
    CheckPixel(rotated, 59, 68, 0, 255);
    zassert_equal(lv_obj_get_width(scene.widget.GetContainer()->GetObject()), 64);
    zassert_equal(lv_obj_get_height(scene.widget.GetContainer()->GetObject()), 24);
    zassert_equal(lv_obj_get_style_transform_pivot_x(presentation, LV_PART_MAIN), lv_pct(50));
    zassert_equal(lv_obj_get_style_transform_pivot_y(presentation, LV_PART_MAIN), lv_pct(50));
    styles.Reset();
    const auto restored = scene.Pixels();
    zassert_mem_equal(baseline.data(), restored.data(), baseline.size() * sizeof(lv_color32_t));
    zassert_false(lv_obj_has_flag(presentation, LV_OBJ_FLAG_OVERFLOW_VISIBLE));
    zassert_false(lv_obj_has_flag(scene.widget.GetContainer()->GetObject(), LV_OBJ_FLAG_OVERFLOW_VISIBLE));
    zassert_false(lv_obj_has_flag(scene.root->GetObject(), LV_OBJ_FLAG_OVERFLOW_VISIBLE));
    zassert_equal(lv_anim_count_running(), count);
}

ZTEST(widget_animations, test_rotation_preserves_external_clipping_at_45_and_90_degrees) {
    ScopedLvglLock lock;
    Scene scene;
    scene.clip->SetHeight(48, true);
    lv_obj_update_layout(scene.root->GetObject());
    PresentationStyles styles(scene.widget);
    styles.Rotate(900);
    auto pixels = scene.Pixels();
    CheckPixel(pixels, 56, 20, 255, 0);
    CheckPixel(pixels, 42, 70, 0, 0);
    styles.Rotate(450);
    pixels = scene.Pixels();
    CheckPixel(pixels, 34, 23, 255, 0);
    CheckPixel(pixels, 59, 68, 0, 0);
    zassert_false(lv_obj_has_flag(scene.clip->GetObject(), LV_OBJ_FLAG_OVERFLOW_VISIBLE));
    zassert_equal(DrawMargin(scene.clip->GetObject()), 0);
}

ZTEST(widget_animations, test_center_pivot_tracks_resize_and_reset_restores_prior_flags_and_margin) {
    ScopedLvglLock lock;
    for(bool layout_overflow : { false, true }) {
        for(bool content_overflow : { false, true }) {
            Scene scene;
            auto* layout = scene.widget.GetContainer()->GetObject();
            auto* content = views_test::WidgetContent(scene.widget);
            lv_obj_set_flag(layout, LV_OBJ_FLAG_OVERFLOW_VISIBLE, layout_overflow);
            lv_obj_set_flag(content, LV_OBJ_FLAG_OVERFLOW_VISIBLE, content_overflow);
            lv_obj_set_ext_draw_size(layout, 3);
            lv_obj_refresh_ext_draw_size(layout);
            const auto callbacks = lv_obj_get_event_count(layout);
            {
                PresentationStyles styles(scene.widget);
                styles.Rotate(450);
                scene.widget.SetPositionPx({ 12, 32 });
                scene.widget.SetSizePx({ 72, 32 });
                lv_obj_update_layout(layout);
                styles.Rotate(900);
                auto pixels = scene.Pixels();
                CheckPixel(pixels, 60, 16, 255, 0);
                CheckPixel(pixels, 48, 58, 0, 255);
                styles.Fade(0);
                styles.Reset();
                styles.Reset();
            }
            zassert_equal(lv_obj_has_flag(layout, LV_OBJ_FLAG_OVERFLOW_VISIBLE), layout_overflow);
            zassert_equal(lv_obj_has_flag(content, LV_OBJ_FLAG_OVERFLOW_VISIBLE), content_overflow);
            zassert_equal(DrawMargin(layout), 3);
            zassert_equal(lv_obj_get_event_count(layout), callbacks);
            zassert_equal(lv_obj_get_style_transform_rotation(content, LV_PART_MAIN), 0);
            zassert_equal(lv_obj_get_style_opa_layered(content, LV_PART_MAIN), LV_OPA_COVER);
        }
    }
}

ZTEST(widget_animations, test_pointer_hits_rotated_button_even_at_zero_presentation_opacity) {
    ScopedLvglLock lock;
    Scene scene;
    lv_obj_add_flag(scene.widget.GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN);
    TestButton button(2, scene.clip, WidgetContext{});
    button.SetPositionPx({ 16, 36 });
    button.SetSizePx({ 64, 24 });
    button.Configure(Configuration());
    zassert_equal(button.Render(), 0);
    button.OnActivated();
    lv_obj_update_layout(scene.root->GetObject());
    auto* object = lv_obj_get_child(views_test::WidgetContent(button), 0);
    std::unique_ptr<lv_group_t, decltype(&lv_group_delete)> group(lv_group_create(), lv_group_delete);
    lv_group_add_obj(group.get(), object);
    PresentationStyles styles(button);
    styles.Rotate(900);
    Pointer pointer;
    pointer.Send(48, 22, LV_INDEV_STATE_PRESSED);
    zassert_true(lv_obj_has_state(object, LV_STATE_PRESSED));
    zassert_equal(lv_group_get_focused(group.get()), object);
    pointer.Send(48, 22, LV_INDEV_STATE_RELEASED);
    zassert_equal(button.clicks, 1);
    pointer.Click(20, 40);
    zassert_equal(button.clicks, 1);
    styles.Fade(0);
    pointer.Click(48, 22);
    zassert_equal(button.clicks, 2);
    button.OnDeactivated();
    pointer.Click(48, 22);
    zassert_equal(button.clicks, 2);
    button.OnActivated();
    scene.clip->SetHeight(20, true);
    lv_obj_update_layout(scene.root->GetObject());
    pointer.Click(48, 22);
    zassert_equal(button.clicks, 2);
    scene.clip->SetHeight(extent, true);
    styles.Reset();
    lv_obj_update_layout(scene.root->GetObject());
    pointer.Click(20, 40);
    zassert_equal(button.clicks, 3);
}

ZTEST(widget_animations, test_live_animated_button_accepts_transformed_and_transparent_input) {
    ScopedLvglLock lock;
    Scene scene;
    lv_obj_add_flag(scene.widget.GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN);
    TestButton button(2, scene.clip, WidgetContext{});
    button.SetPositionPx({ 16, 36 });
    button.SetSizePx({ 64, 24 });
    button.Configure(AnimationConfiguration());
    zassert_equal(button.Render(), 0);
    button.OnActivated();
    lv_obj_update_layout(scene.root->GetObject());
    auto* content = views_test::WidgetContent(button);
    auto* object = lv_obj_get_child(content, 0);
    std::unique_ptr<lv_group_t, decltype(&lv_group_delete)> group(lv_group_create(), lv_group_delete);
    lv_group_add_obj(group.get(), object);
    Advance(250);
    const auto attempts = animation_start_calls;
    Pointer pointer;
    pointer.Click(48, 22);
    zassert_equal(button.clicks, 1);
    zassert_equal(lv_group_get_focused(group.get()), object);
    pointer.Click(20, 40);
    zassert_equal(button.clicks, 1);
    zassert_equal(animation_start_calls, attempts);
    Publish(WidgetPropertyType::ANIMATION_TYPE, 1);
    Advance(500);
    zassert_equal(lv_obj_get_style_opa_layered(content, LV_PART_MAIN), 0);
    pointer.Click(20, 40);
    zassert_equal(button.clicks, 2);
    zassert_equal(animation_start_calls, attempts + 1);
    Publish(WidgetPropertyType::IS_ACTIVE, false);
    pointer.Click(20, 40);
    zassert_equal(button.clicks, 2);
    Publish(WidgetPropertyType::IS_ACTIVE, true);
    pointer.Click(20, 40);
    zassert_equal(button.clicks, 3);
}

ZTEST(widget_animations, test_presentation_preserves_button_bubbling_and_slider_gesture_consumption) {
    ScopedLvglLock lock;
    for(bool slider : { false, true }) {
        Scene scene;
        lv_obj_add_flag(scene.widget.GetContainer()->GetObject(), LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(scene.root->GetObject(), LV_OBJ_FLAG_GESTURE_BUBBLE);
        int gestures = 0;
        lv_obj_add_event_cb(scene.root->GetObject(), [](lv_event_t* event) {
            ++*static_cast<int*>(lv_event_get_user_data(event));
        }, LV_EVENT_GESTURE, &gestures);
        std::unique_ptr<WidgetBase> control;
        if(slider)
            control = std::make_unique<controls::SliderControl>(2, scene.clip, WidgetContext{});
        else
            control = std::make_unique<TestButton>(2, scene.clip, WidgetContext{});
        control->SetPositionPx({ 16, 36 });
        control->SetSizePx({ 64, 24 });
        control->Configure(Configuration());
        zassert_equal(control->Render(), 0);
        control->OnActivated();
        lv_obj_update_layout(scene.root->GetObject());
        Pointer pointer;
        pointer.Send(22, 48, LV_INDEV_STATE_PRESSED);
        pointer.Send(45, 48, LV_INDEV_STATE_PRESSED);
        pointer.Send(70, 48, LV_INDEV_STATE_PRESSED);
        pointer.Send(70, 48, LV_INDEV_STATE_RELEASED);
        zassert_equal(gestures, slider ? 0 : 1);
    }
}

ZTEST(widget_animations, test_overlapping_children_compose_presentation_and_user_opacity_once) {
    ScopedLvglLock lock;
    Scene scene;
    auto* presentation = views_test::WidgetContent(scene.widget);
    for(int index = 0; index < 2; ++index) {
        auto* rectangle = lv_obj_get_child(presentation, index);
        lv_obj_set_style_bg_color(rectangle, lv_color_hex(0xFF0000), 0);
        lv_obj_set_pos(rectangle, index == 0 ? 4 : 12, index == 0 ? 4 : 8);
        lv_obj_set_size(rectangle, 28, 14);
    }
    PresentationStyles styles(scene.widget);
    lv_obj_set_style_opa_layered(scene.widget.GetContainer()->GetObject(), 128, 0);
    styles.Fade(128);
    auto pixels = scene.Pixels();
    CheckPixel(pixels, 22, 43, 64, 0);
    CheckPixel(pixels, 32, 48, 64, 0);
    styles.Fade(0);
    pixels = scene.Pixels();
    CheckPixel(pixels, 32, 48, 0, 0);
    zassert_true(scene.widget.IsProcessingEligible());
    styles.Reset();
    pixels = scene.Pixels();
    CheckPixel(pixels, 32, 48, 128, 0);
    zassert_equal(lv_obj_get_style_opa_layered(scene.widget.GetContainer()->GetObject(), LV_PART_MAIN), 128);
}
