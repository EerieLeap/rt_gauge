#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <lvgl_mem.h>
#include <zephyr/sys/mem_stats.h>
#include <zephyr/ztest.h>

#include "domain/ui_domain/lvgl_lock.h"
#include "domain/ui_domain/models/icon_type.h"
#include "event_bus/event_channels.h"
#include "subsys/assets/asset_kind.h"
#include "views/widgets/basic/icon_widget/icon_widget.h"
#include "views/widgets/basic/icons/icon_factory.h"

#include "views_test_support.h"

using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::widgets;
using namespace eerie_leap::views::widgets::basic;
using eerie_leap::domain::ui_domain::ScopedLvglLock;
using eerie_leap::subsys::assets::AssetKind;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::widgets::basic::icons::IconFactory;

namespace {

// Opaque red on the left half, half transparent blue on the right.
constexpr std::string_view HALVES_SVG = R"svg(<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" width="40" height="20">
  <rect x="0" y="0" width="20" height="20" fill="#ff0000"/>
  <rect x="20" y="0" width="20" height="20" fill="#0000ff" fill-opacity="0.5"/>
</svg>)svg";

// A 2:1 document sized by its viewBox alone, filled green.
constexpr std::string_view WIDE_SVG = R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 4 2">
  <rect width="4" height="2" fill="#00ff00"/>
</svg>)svg";

// Text needs FreeType and a raster image needs a loader, neither of which the unit has: both are skipped.
constexpr std::string_view UNSUPPORTED_SVG = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="10" height="10">
  <text x="0" y="8">hi</text>
  <image href="logo.png" width="10" height="10"/>
  <rect width="10" height="10" fill="#00ff00"/>
</svg>)svg";

constexpr std::string_view NO_SIZE_SVG =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg"><rect width="4" height="4"/></svg>)svg";

// Red on the left, blue on the right: a fill may refer to a gradient.
constexpr std::string_view GRADIENT_SVG = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="20" height="10">
  <linearGradient id="fade"><stop offset="0" stop-color="#ff0000"/><stop offset="1" stop-color="#0000ff"/></linearGradient>
  <rect width="20" height="10" fill="url(#fade)"/>
</svg>)svg";

// What LVGL would crash on or draw without bound: a <use> without a reference, a fill that refers to a shape, and
// dashes a thousandth of a pixel long.
constexpr std::string_view USE_SVG = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
  <rect id="box" width="4" height="4"/><use/>
</svg>)svg";
constexpr std::string_view SHAPE_PAINT_SVG = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
  <rect id="box" width="4" height="4"/><rect width="4" height="4" fill="url(#box)"/>
</svg>)svg";
constexpr std::string_view DASHED_SVG = R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">
  <line x2="4" y2="4" stroke="#000000" style="stroke-dasharray:0.001 0.001"/>
</svg>)svg";

// A viewBox smaller than a pixel.
constexpr std::string_view TINY_SVG =
    R"svg(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 0.5 0.5"><rect width="0.5" height="0.5"/></svg>)svg";

std::string Nested(int depth, std::string_view prefix = "") {
    std::string svg = R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4">)";
    svg += prefix;
    for(int level = 1; level < depth; ++level)
        svg += "<g>";
    svg += R"(<rect width="4" height="4" fill="#00ff00"/>)";
    for(int level = 1; level < depth; ++level)
        svg += "</g>";
    return svg + "</svg>";
}

std::shared_ptr<Frame> MakeRoot() {
    return std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(100, true).SetHeight(100, true).Build());
}

WidgetContext Context() {
    auto store = views_test::MakeAssetStore("svg-icon");
    zassert_true(store->Save("halves.svg", views_test::TextBytes(HALVES_SVG)));
    zassert_true(store->Save("wide.svg", views_test::TextBytes(WIDE_SVG)));
    zassert_true(store->Save("unsupported.svg", views_test::TextBytes(UNSUPPORTED_SVG)));
    zassert_true(store->Save("no_size.svg", views_test::TextBytes(NO_SIZE_SVG)));
    zassert_true(store->Save("text.svg", views_test::TextBytes("not an svg document")));
    zassert_true(store->Save("gradient.svg", views_test::TextBytes(GRADIENT_SVG)));
    zassert_true(store->Save("use.svg", views_test::TextBytes(USE_SVG)));
    zassert_true(store->Save("shape_paint.svg", views_test::TextBytes(SHAPE_PAINT_SVG)));
    zassert_true(store->Save("dashed.svg", views_test::TextBytes(DASHED_SVG)));
    zassert_true(store->Save("tiny.svg", views_test::TextBytes(TINY_SVG)));
    zassert_true(store->Save("deepest.svg", views_test::TextBytes(Nested(6))));
    zassert_true(store->Save("too_deep.svg", views_test::TextBytes(Nested(7))));
    // An ignored element takes stray end tags without closing anything, so they must not count against the depth.
    zassert_true(store->Save("hidden_depth.svg", views_test::TextBytes(Nested(7, "<foo></g></g></g></g></foo>"))));
    // Nothing closes the root: what follows it is parsed into it, here one level deeper than allowed.
    std::string after_root = Nested(1);
    for(int level = 1; level < 7; ++level)
        after_root += "<g>";
    for(int level = 1; level < 7; ++level)
        after_root += "</g>";
    zassert_true(store->Save("after_root.svg", views_test::TextBytes(after_root)));

    // A drawable document one byte larger than the LVGL heap's free space allows.
    sys_memory_stats heap{};
    lvgl_heap_stats(&heap);
    std::string large(WIDE_SVG);
    large.insert(large.size() - std::string_view("</svg>").size(), heap.free_bytes / 8 + 1 - large.size(), ' ');
    zassert_true(store->Save("large.svg", views_test::TextBytes(large)));
    // A small document of more shapes than the LVGL heap's free space allows.
    std::string many(Nested(1));
    for(size_t element = 0; element <= heap.free_bytes / 1536; ++element)
        many.insert(many.size() - std::string_view("</svg>").size(), R"(<rect width="1" height="1"/>)");
    zassert_true(store->Save("many.svg", views_test::TextBytes(many)));

    return WidgetContext{ .assets_manager = views_test::MakeAssetsManager(AssetKind::IMAGES, std::move(store)) };
}

std::shared_ptr<WidgetConfiguration> Configuration(const char* file_path) {
    auto configuration = std::make_shared<WidgetConfiguration>(std::allocator_arg, std::pmr::get_default_resource());
    configuration->type = WidgetType::BasicIcon;
    configuration->id = 1;
    configuration->properties[WidgetPropertyType::ICON_TYPE] = static_cast<int>(IconType::Svg);
    configuration->properties[WidgetPropertyType::FILE_PATH] = std::pmr::string(file_path);
    return configuration;
}

const lv_image_dsc_t& Image(const IconWidget& widget) {
    auto* image = widget.GetIconContainer()->GetObject();
    zassert_true(lv_obj_check_type(image, &lv_image_class));
    return *static_cast<const lv_image_dsc_t*>(lv_image_get_src(image));
}

lv_color32_t Pixel(const lv_image_dsc_t& image, uint32_t x, uint32_t y) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): an ARGB8888 image's bytes are LVGL pixels
    return reinterpret_cast<const lv_color32_t*>(image.data + (y * image.header.stride))[x];
}

// ThorVG's 8-bit blending leaves a fully covered pixel at 254.
bool IsOpaque(lv_color32_t pixel) {
    return pixel.alpha >= LV_OPA_COVER - 1;
}

void CheckSize(std::shared_ptr<WidgetConfiguration> configuration, uint32_t width, uint32_t height) {
    IconWidget widget(1, MakeRoot(), Context());
    widget.Configure(std::move(configuration));
    zassert_equal(widget.Render(), 0);
    const auto& image = Image(widget);
    zassert_equal(image.header.w, width, "width %u != %u", image.header.w, width);
    zassert_equal(image.header.h, height, "height %u != %u", image.header.h, height);
}

bool Renders(std::shared_ptr<WidgetConfiguration> configuration) {
    IconWidget widget(1, MakeRoot(), Context());
    widget.Configure(std::move(configuration));
    const bool rendered = widget.Render() == 0;
    zassert_equal(widget.GetIconContainer() != nullptr, rendered);
    return rendered;
}

void* Setup() {
    views_test::EnsureTestDisplay();
    eerie_leap::event_bus::InitializeEventChannels();
    return nullptr;
}

} // namespace

ZTEST_SUITE(svg_icon, NULL, Setup, NULL, views_test::CleanTestDisplay, NULL);

ZTEST(svg_icon, test_svg_is_an_icon_type_with_its_own_properties) {
    zassert_true(IconFactory::GetInstance().IsAvailable(IconType::Svg));

    ScopedLvglLock lock;
    IconWidget widget(1, MakeRoot(), WidgetContext{}, IconType::Svg);
    const auto supported = widget.GetSupportedProperties();
    const auto supports = [&supported](WidgetPropertyType type) {
        return std::ranges::find(supported, type) != supported.end();
    };
    zassert_true(supports(WidgetPropertyType::FILE_PATH));
    zassert_true(supports(WidgetPropertyType::WIDTH_PX));
    zassert_true(supports(WidgetPropertyType::HEIGHT_PX));
    zassert_false(supports(WidgetPropertyType::IMG_WIDTH));
}

ZTEST(svg_icon, test_document_renders_at_its_own_size_in_straight_alpha) {
    ScopedLvglLock lock;
    IconWidget widget(1, MakeRoot(), Context());
    widget.Configure(Configuration("halves.svg"));
    zassert_equal(widget.Render(), 0);

    const auto& image = Image(widget);
    zassert_equal(image.header.cf, LV_COLOR_FORMAT_ARGB8888);
    zassert_equal(image.header.w, 40);
    zassert_equal(image.header.h, 20);

    const auto red = Pixel(image, 10, 10);
    zassert_true(IsOpaque(red));
    zassert_true(red.red >= 250 && red.green <= 5 && red.blue <= 5);

    // L8 targets cannot blend premultiplied pixels, so the colour must stay unscaled.
    const auto blue = Pixel(image, 30, 10);
    zassert_within(blue.alpha, 128, 4, "alpha %d", blue.alpha);
    zassert_true(blue.blue >= 245 && blue.red <= 5 && blue.green <= 5, "pixel is premultiplied");
}

ZTEST(svg_icon, test_size_properties_fit_the_document_keeping_its_aspect_ratio) {
    ScopedLvglLock lock;
    CheckSize(Configuration("wide.svg"), 4, 2);

    auto configuration = Configuration("wide.svg");
    configuration->properties[WidgetPropertyType::WIDTH_PX] = 80;
    CheckSize(configuration, 80, 40);

    configuration = Configuration("wide.svg");
    configuration->properties[WidgetPropertyType::HEIGHT_PX] = 10.0;
    CheckSize(configuration, 20, 10);

    configuration = Configuration("tiny.svg");
    configuration->properties[WidgetPropertyType::WIDTH_PX] = 10;
    CheckSize(configuration, 10, 10);

    // Both set: the document is fitted, centered, and the rest stays transparent.
    configuration = Configuration("wide.svg");
    configuration->properties[WidgetPropertyType::WIDTH_PX] = 80;
    configuration->properties[WidgetPropertyType::HEIGHT_PX] = 80;
    IconWidget widget(1, MakeRoot(), Context());
    widget.Configure(configuration);
    zassert_equal(widget.Render(), 0);
    const auto& image = Image(widget);
    zassert_equal(image.header.w, 80);
    zassert_equal(image.header.h, 80);
    zassert_equal(Pixel(image, 40, 10).alpha, 0);
    zassert_equal(Pixel(image, 40, 70).alpha, 0);
    const auto green = Pixel(image, 40, 40);
    zassert_true(IsOpaque(green));
    zassert_true(green.green >= 250 && green.red <= 5);
    zassert_true(IsOpaque(Pixel(image, 0, 21)));
    zassert_true(IsOpaque(Pixel(image, 79, 58)));
}

ZTEST(svg_icon, test_unsupported_elements_are_skipped) {
    ScopedLvglLock lock;
    IconWidget widget(1, MakeRoot(), Context());
    widget.Configure(Configuration("unsupported.svg"));
    zassert_equal(widget.Render(), 0);
    const auto green = Pixel(Image(widget), 5, 5);
    zassert_true(IsOpaque(green));
    zassert_true(green.green >= 250 && green.red <= 5 && green.blue <= 5);
}

ZTEST(svg_icon, test_nesting_is_bounded_before_parsing) {
    ScopedLvglLock lock;
    zassert_true(Renders(Configuration("deepest.svg")));
    zassert_false(Renders(Configuration("too_deep.svg")));
    zassert_false(Renders(Configuration("hidden_depth.svg")));
    zassert_false(Renders(Configuration("after_root.svg")));
}

ZTEST(svg_icon, test_documents_lvgl_cannot_draw_safely_render_nothing) {
    ScopedLvglLock lock;
    for(const char* file_path : { "use.svg", "shape_paint.svg", "dashed.svg" })
        zassert_false(Renders(Configuration(file_path)), "%s rendered", file_path);

    // A fill that refers to a gradient is drawn.
    IconWidget widget(1, MakeRoot(), Context());
    widget.Configure(Configuration("gradient.svg"));
    zassert_equal(widget.Render(), 0);
    const auto left = Pixel(Image(widget), 1, 5);
    const auto right = Pixel(Image(widget), 18, 5);
    zassert_true(IsOpaque(left) && IsOpaque(right));
    zassert_true(left.red > 200 && left.blue < 55, "left (%d, %d)", left.red, left.blue);
    zassert_true(right.blue > 200 && right.red < 55, "right (%d, %d)", right.red, right.blue);
}

ZTEST(svg_icon, test_unusable_documents_and_sizes_render_nothing) {
    ScopedLvglLock lock;
    for(const char* file_path : { "", "missing.svg", "text.svg", "no_size.svg", "large.svg", "many.svg" })
        zassert_false(Renders(Configuration(file_path)), "%s rendered", file_path);

    for(const ConfigValue& size : { ConfigValue{ -1 }, ConfigValue{ 1e20 }, ConfigValue{ 40000 } }) {
        auto configuration = Configuration("wide.svg");
        configuration->properties[WidgetPropertyType::WIDTH_PX] = size;
        zassert_false(Renders(configuration));
    }

    // 4096 x 2048 ARGB8888 pixels take 32 MiB, past the 4 MiB limit.
    auto configuration = Configuration("wide.svg");
    configuration->properties[WidgetPropertyType::WIDTH_PX] = 4096;
    zassert_false(Renders(configuration));

    // 16384 x 1 pixels fit in 64 KiB, but their stride does not fit an image header's 16 bits.
    configuration = Configuration("wide.svg");
    configuration->properties[WidgetPropertyType::WIDTH_PX] = 16384;
    configuration->properties[WidgetPropertyType::HEIGHT_PX] = 1;
    zassert_false(Renders(configuration));
}
