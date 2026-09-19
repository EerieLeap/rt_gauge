#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <vector>

#include <zephyr/ztest.h>

#include "domain/sensor_domain/event_bus/sensor_events_channel.h"
#include "event_bus/event_channel_id.h"
#include "event_bus/event_channels.h"
#include "views/widgets/basic/icons/dot_icon/dot_icon.h"
#include "views/widgets/controls/button_control/button_control.h"
#include "views/widgets/controls/slider_control/slider_control.h"
#include "views/widgets/widget_base.h"

#include "views_test_support.h"

using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::domain::sensor_domain::event_bus;
using namespace eerie_leap::views::widgets;
using eerie_leap::domain::ui_domain::ScopedLvglLock;
using eerie_leap::event_bus::EventChannelId;
using eerie_leap::views::themes::ITheme;
using eerie_leap::views::utilitites::Frame;

namespace {

constexpr int extent = 96;

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

class TestDot : public basic::icons::DotIcon {
public:
    using DotIcon::DotIcon;
    using IconBase::IsProcessingEligible;
};

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

ZTEST_SUITE(widget_animations, NULL, Setup, NULL, views_test::CleanTestDisplay, NULL);

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
    TestDot::RegisterProperties(*properties);
    TestDot dot(scene.widget.Content());
    dot.Configure(properties);
    zassert_equal(dot.Render(), 0);
    dot.SetProcessingEnabled(true);
    auto check = [&](bool eligible) {
        zassert_equal(scene.widget.IsProcessingEligible(), eligible);
        zassert_equal(descendant.IsProcessingEligible(), eligible);
        zassert_equal(dot.IsProcessingEligible(), eligible);
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
