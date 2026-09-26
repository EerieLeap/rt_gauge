#include <cstdint>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <zephyr/ztest.h>

#include "subsys/assets/assets_manager.h"
#include "subsys/device_tree/dt_fs.h"
#include "subsys/fs/services/fs_service.h"
#include "domain/ui_domain/lvgl_lock.h"
#include "event_bus/event_channels.h"
#include "views/assets/animations/animations_register.h"
#include "views/widgets/basic/lottie_widget/lottie_widget.h"
#include "views/widgets/widget_factory.h"

#include "views_test_support.h"

using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::widgets;
using namespace eerie_leap::views::widgets::basic;
using eerie_leap::domain::ui_domain::ScopedLvglLock;
using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::views::assets::animations::ui_lottie_spinner_json;
using eerie_leap::views::utilitites::Frame;

namespace {

// A 32x32 white square whose layer opacity rises linearly from 0 to 100 over one second,
// authored at 30 fps so that playing it at LVGL's assumed 60 fps would show.
constexpr std::string_view fade_json = R"json({
  "v": "5.7.4", "fr": 30, "ip": 0, "op": 30, "w": 32, "h": 32, "nm": "fade", "ddd": 0, "assets": [],
  "layers": [{
    "ddd": 0, "ind": 1, "ty": 4, "nm": "square", "sr": 1, "ao": 0, "ip": 0, "op": 30, "st": 0, "bm": 0,
    "ks": {
      "o": { "a": 1, "k": [
        { "t": 0, "s": [0], "i": { "x": [1], "y": [1] }, "o": { "x": [0], "y": [0] } },
        { "t": 30, "s": [100] }
      ] },
      "r": { "a": 0, "k": 0 },
      "p": { "a": 0, "k": [16, 16, 0] },
      "a": { "a": 0, "k": [16, 16, 0] },
      "s": { "a": 0, "k": [100, 100, 100] }
    },
    "shapes": [{
      "ty": "gr", "nm": "box",
      "it": [
        { "ty": "rc", "nm": "rect", "d": 1, "p": { "a": 0, "k": [16, 16] }, "s": { "a": 0, "k": [32, 32] },
          "r": { "a": 0, "k": 0 } },
        { "ty": "fl", "nm": "fill", "c": { "a": 0, "k": [1, 1, 1, 1] }, "o": { "a": 0, "k": 100 }, "r": 1 },
        { "ty": "tr", "p": { "a": 0, "k": [0, 0] }, "a": { "a": 0, "k": [0, 0] },
          "s": { "a": 0, "k": [100, 100] }, "r": { "a": 0, "k": 0 }, "o": { "a": 0, "k": 100 } }
      ]
    }]
  }]
})json";

std::shared_ptr<Frame> MakeRoot() {
    return std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(100, true).SetHeight(100, true).Build());
}

std::span<const uint8_t> Bytes(std::string_view text) {
    return { reinterpret_cast<const uint8_t*>(text.data()), text.size() };
}

WidgetContext Context() {
    using eerie_leap::subsys::device_tree::DtFs;
    using eerie_leap::subsys::fs::services::FsService;
    DtFs::InitInternalFs();
    auto fs = std::make_shared<FsService>(DtFs::GetInternalFsMp());
    zassert_true(fs->Initialize());
    auto assets = std::make_shared<AssetsManager>(fs, "lottie-widget");
    zassert_true(assets->Save("spinner.json", Bytes(ui_lottie_spinner_json)));
    zassert_true(assets->Save("fade.json", Bytes(fade_json)));
    zassert_true(assets->Save("broken.json", Bytes("{\"v\":\"5.7.4\"")));
    return WidgetContext { .assets_manager = std::move(assets) };
}

std::shared_ptr<WidgetConfiguration> Configuration(const char* file_path, int width = 0, int height = 0) {
    auto configuration = std::make_shared<WidgetConfiguration>(std::allocator_arg, std::pmr::get_default_resource());
    configuration->type = WidgetType::BasicLottie;
    configuration->id = 1;
    configuration->properties[WidgetPropertyType::FILE_PATH] = std::pmr::string(file_path);
    if(width > 0)
        configuration->properties[WidgetPropertyType::WIDTH_PX] = width;
    if(height > 0)
        configuration->properties[WidgetPropertyType::HEIGHT_PX] = height;
    return configuration;
}

lv_obj_t* LottieObject(const LottieWidget& widget) {
    auto* content = views_test::WidgetContent(widget);
    return lv_obj_get_child_count(content) == 1 ? lv_obj_get_child(content, 0) : nullptr;
}

lv_color32_t Pixel(lv_obj_t* animation, int x, int y) {
    auto* buffer = lv_canvas_get_draw_buf(animation);
    return reinterpret_cast<const lv_color32_t*>(buffer->data + y * buffer->header.stride)[x];
}

std::vector<uint8_t> Pixels(lv_obj_t* animation) {
    auto* buffer = lv_canvas_get_draw_buf(animation);
    return { buffer->data, buffer->data + buffer->header.stride * buffer->header.h };
}

bool IsPaused(lv_obj_t* animation) {
    return lv_anim_is_paused(lv_lottie_get_anim(animation));
}

void Tick(uint32_t elapsed) {
    lv_tick_inc(elapsed);
    lv_anim_refr_now();
}

bool IsRejected(const WidgetConfiguration& configuration) {
    try {
        WidgetFactory::GetInstance().ValidateChildren(configuration, {});
    } catch(const std::invalid_argument&) {
        return true;
    }
    return false;
}

void CheckFade(lv_obj_t* animation, int alpha) {
    const auto pixel = Pixel(animation, 16, 16);
    zassert_within(pixel.alpha, alpha, 8, "alpha %d != %d", pixel.alpha, alpha);
    // L8 targets cannot blend premultiplied pixels, so the colour must stay unscaled.
    if(alpha > 0)
        zassert_true(pixel.red >= 245 && pixel.green >= 245 && pixel.blue >= 245, "pixel is premultiplied");
}

void* Setup() {
    views_test::EnsureTestDisplay();
    eerie_leap::event_bus::InitializeEventChannels();
    return nullptr;
}

} // namespace

ZTEST_SUITE(lottie_widget, NULL, Setup, NULL, views_test::CleanTestDisplay, NULL);

ZTEST(lottie_widget, test_bundled_spinner_renders_at_widget_size_in_straight_alpha) {
    ScopedLvglLock lock;
    LottieWidget widget(1, MakeRoot(), Context());
    widget.Configure(Configuration("spinner.json"));
    zassert_equal(widget.Render(), 0);

    auto* animation = LottieObject(widget);
    zassert_not_null(animation);
    auto* buffer = lv_canvas_get_draw_buf(animation);
    zassert_equal(buffer->header.w, 100);
    zassert_equal(buffer->header.h, 100);
    zassert_equal(buffer->header.cf, LV_COLOR_FORMAT_ARGB8888);
    zassert_false(lv_draw_buf_has_flag(buffer, LV_IMAGE_FLAGS_PREMULTIPLIED));
    // 120 frames at the authored 60 fps.
    zassert_equal(lv_anim_get_time(lv_lottie_get_anim(animation)), 2000);

    // The ring is drawn before the widget ever plays; its middle stays empty.
    zassert_true(Pixel(animation, 50, 13).alpha > 0);
    zassert_equal(Pixel(animation, 50, 50).alpha, 0);

    lv_obj_update_layout(lv_screen_active());
    widget.OnActivated();
    const auto first = Pixels(animation);
    Tick(0);
    Tick(500);
    zassert_false(Pixels(animation) == first, "the spinner did not advance");
}

ZTEST(lottie_widget, test_playback_follows_processing_at_the_authored_frame_rate) {
    ScopedLvglLock lock;
    LottieWidget widget(1, MakeRoot(), Context());
    widget.Configure(Configuration("fade.json", 32, 32));
    zassert_equal(widget.Render(), 0);
    lv_obj_update_layout(lv_screen_active());

    auto* animation = LottieObject(widget);
    zassert_not_null(animation);
    zassert_equal(lv_anim_get_time(lv_lottie_get_anim(animation)), 1000);
    zassert_true(IsPaused(animation));
    CheckFade(animation, 0);
    Tick(250);
    CheckFade(animation, 0);

    widget.OnActivated();
    zassert_false(IsPaused(animation));
    Tick(250);
    // Frame 7 of 30.
    CheckFade(animation, 59);

    widget.OnDeactivated();
    zassert_true(IsPaused(animation));
    Tick(500);
    CheckFade(animation, 59);

    widget.OnActivated();
    Tick(250);
    // Frame 15 of 30.
    CheckFade(animation, 127);
}

ZTEST(lottie_widget, test_rerender_replaces_the_animation) {
    ScopedLvglLock lock;
    LottieWidget widget(1, MakeRoot(), Context());
    widget.Configure(Configuration("fade.json", 32, 32));
    zassert_equal(widget.Render(), 0);
    auto* first = LottieObject(widget);
    zassert_not_null(first);

    widget.OnActivated();
    zassert_equal(widget.Render(), 0);
    auto* second = LottieObject(widget);
    zassert_not_null(second);
    zassert_equal(lv_obj_get_child_count(views_test::WidgetContent(widget)), 1);
    zassert_false(IsPaused(second));
}

ZTEST(lottie_widget, test_missing_or_invalid_asset_fails_render) {
    ScopedLvglLock lock;
    auto context = Context();
    for(const char* file_path : { "missing.json", "broken.json" }) {
        LottieWidget widget(1, MakeRoot(), context);
        widget.Configure(Configuration(file_path));
        zassert_not_equal(widget.Render(), 0, "%s rendered", file_path);
        zassert_is_null(LottieObject(widget));
    }

    LottieWidget widget(1, MakeRoot(), WidgetContext{});
    widget.Configure(Configuration("spinner.json"));
    zassert_not_equal(widget.Render(), 0);
}

ZTEST(lottie_widget, test_validation_requires_a_file_path) {
    auto configuration = Configuration("spinner.json");
    zassert_false(IsRejected(*configuration));

    for(auto value : { ConfigValue { std::pmr::string() }, ConfigValue { 1 } }) {
        configuration->properties[WidgetPropertyType::FILE_PATH] = value;
        zassert_true(IsRejected(*configuration));
    }

    configuration->properties.erase(WidgetPropertyType::FILE_PATH);
    zassert_true(IsRejected(*configuration));
}
