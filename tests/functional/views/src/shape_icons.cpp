#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>

#include <zephyr/ztest.h>
#include <libs/thorvg/thorvg_capi.h>

#include "domain/sensor_domain/event_bus/sensor_events_channel.h"
#include "domain/ui_domain/models/widget_direction.h"
#include "domain/ui_domain/models/widget_fill_mode.h"
#include "event_bus/event_channels.h"
#include "views/overlay_host.h"
#include "views/themes/default_theme.h"
#include "views/widgets/basic/arc_icon_widget/arc_icon_widget.h"
#include "views/widgets/basic/icon_widget/icon_widget.h"
#include "views/widgets/basic/icons/icon_factory.h"
#include "views/widgets/basic/icons/shape_icon/polygon_icon_base.h"
#include "views/widgets/indicators/bar_indicator/bar_indicator.h"

#include "views_test_support.h"

using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::widgets;
using namespace eerie_leap::views::widgets::basic;
using namespace eerie_leap::views::widgets::basic::icons;
using namespace eerie_leap::views::themes;
using eerie_leap::domain::sensor_domain::event_bus::SensorEventsChannel;
using eerie_leap::domain::sensor_domain::event_bus::SensorEventType;
using eerie_leap::domain::sensor_domain::event_bus::SensorPayloadType;
using eerie_leap::event_bus::EventChannelId;
using eerie_leap::event_bus::InitializeEventChannels;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::widgets::indicators::BarIndicator;

namespace {

constexpr std::array shape_types {
    IconType::Rectangle,
    IconType::TriangleIsosceles,
    IconType::TriangleRight,
    IconType::Oval,
    IconType::Line
};

std::shared_ptr<Frame> MakeRoot() {
    return std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(200, true).SetHeight(200, true).Build());
}

std::shared_ptr<WidgetConfiguration> Configuration(
    IconType type,
    int width_px = 64,
    int height_px = 48,
    WidgetFillMode fill = WidgetFillMode::Filled,
    int radius_px = 0,
    int stroke_px = 4,
    WidgetDirection direction = WidgetDirection::LeftToRight) {

    auto configuration = std::make_shared<WidgetConfiguration>(std::allocator_arg, std::pmr::get_default_resource());
    configuration->type = WidgetType::BasicIcon;
    configuration->id = 1;
    configuration->properties[WidgetPropertyType::ICON_TYPE] = static_cast<int>(type);
    configuration->properties[WidgetPropertyType::WIDTH_PX] = width_px;
    configuration->properties[WidgetPropertyType::HEIGHT_PX] = height_px;
    configuration->properties[WidgetPropertyType::STROKE_PX] = stroke_px;
    if(type == IconType::Line)
        configuration->properties[WidgetPropertyType::DIRECTION] = static_cast<int>(direction);
    else
        configuration->properties[WidgetPropertyType::FILL_MODE] = static_cast<int>(fill);
    if(type == IconType::Rectangle || type == IconType::TriangleIsosceles || type == IconType::TriangleRight)
        configuration->properties[WidgetPropertyType::CORNER_RAD_PX] = radius_px;
    return configuration;
}

std::unique_ptr<IconWidget> MakeWidget(std::shared_ptr<WidgetConfiguration> configuration) {
    auto widget = std::make_unique<IconWidget>(1, MakeRoot(), WidgetContext {});
    widget->Configure(std::move(configuration));
    zassert_equal(widget->Render(), 0);
    widget->OnActivated();
    return widget;
}

lv_obj_t* ImageObject(const IconWidget& widget) {
    return widget.GetContainer()->GetChild()->GetObject();
}

const lv_image_dsc_t& Mask(const IconWidget& widget) {
    auto image = static_cast<const lv_image_dsc_t*>(lv_image_get_src(ImageObject(widget)));
    zassert_not_null(image);
    zassert_equal(image->header.cf, LV_COLOR_FORMAT_A8);
    return *image;
}

uint8_t Alpha(const IconWidget& widget, int x, int y) {
    const auto& image = Mask(widget);
    zassert_true(x >= 0 && x < image.header.w && y >= 0 && y < image.header.h);
    return image.data[y * image.header.stride + x];
}

std::vector<uint8_t> Pixels(const IconWidget& widget) {
    const auto& image = Mask(widget);
    return { image.data, image.data + image.data_size };
}

std::vector<uint8_t> PolygonPixels(std::vector<lv_fpoint_t> vertices,
    WidgetFillMode fill = WidgetFillMode::Filled, int radius = 0, int stroke = 4) {
    // A test-only shape demonstrates reuse without adding a new public icon type.
    class TestPolygonIcon : public PolygonIconBase {
        Polygon vertices_;
        Polygon GetVertices(float, float) const override { return vertices_; }

    public:
        TestPolygonIcon(std::shared_ptr<Frame> parent, Polygon vertices)
            : PolygonIconBase(std::move(parent)), vertices_(std::move(vertices)) {}
        IconType GetIconType() const override { return IconType::None; }
    };
    TestPolygonIcon icon(MakeRoot(), std::move(vertices));
    auto properties = std::make_shared<WidgetPropertyStore>();
    TestPolygonIcon::RegisterProperties(*properties);
    properties->Set(WidgetPropertyType::WIDTH_PX, 64);
    properties->Set(WidgetPropertyType::HEIGHT_PX, 64);
    properties->Set(WidgetPropertyType::FILL_MODE, static_cast<int>(fill));
    properties->Set(WidgetPropertyType::CORNER_RAD_PX, radius);
    properties->Set(WidgetPropertyType::STROKE_PX, stroke);
    icon.Configure(properties);
    zassert_equal(icon.Render(), 0);
    auto image = static_cast<const lv_image_dsc_t*>(lv_image_get_src(icon.GetContainer()->GetObject()));
    zassert_not_null(image);
    std::vector<uint8_t> pixels(64 * 64);
    for(int y = 0; y < 64; ++y)
        std::copy_n(image->data + y * image->header.stride, 64, pixels.data() + y * 64);
    return pixels;
}

void SavePreview(const lv_draw_buf_t& snapshot, const char* name, int variant) {
#ifdef CONFIG_ARCH_POSIX
    const char* directory = std::getenv("SHAPE_ICON_PREVIEW_DIR");
    if(directory == nullptr)
        return;
    char path[512];
    std::snprintf(path, sizeof(path), "%s/%s_%d.ppm", directory, name, variant);
    auto file = std::fopen(path, "wb");
    zassert_not_null(file);
    std::fprintf(file, "P6\n%u %u\n255\n", snapshot.header.w, snapshot.header.h);
    for(uint32_t y = 0; y < snapshot.header.h; ++y) {
        auto pixels = reinterpret_cast<const lv_color32_t*>(snapshot.data + y * snapshot.header.stride);
        for(uint32_t x = 0; x < snapshot.header.w; ++x) {
            uint8_t rgb[] { pixels[x].red, pixels[x].green, pixels[x].blue };
            zassert_equal(std::fwrite(rgb, 1, sizeof(rgb), file), sizeof(rgb));
        }
    }
    zassert_equal(std::fclose(file), 0);
#endif
}

void Bind(WidgetConfiguration& configuration, WidgetPropertyType type) {
    configuration.bindings.push_back(PropertyBinding {
        .target = type,
        .channel = EventChannelId::Sensors,
        .event_type = std::to_underlying(SensorEventType::DataUpdated),
        .payload_key = std::to_underlying(SensorPayloadType::Value)
    });
}

void Publish(float value) {
    SensorEventsChannel::GetInstance().Publish({
        .source_id = 0,
        .type = SensorEventType::DataUpdated,
        .payload = {{ SensorPayloadType::Value, value }}
    });
}

void* SetUp() {
    views_test::EnsureTestDisplay();
    InitializeEventChannels();
    return nullptr;
}

void Clean(void* fixture) {
    ThemeManager::GetInstance().SetTheme(std::make_shared<DefaultTheme>());
    views_test::CleanTestDisplay(fixture);
}

} // namespace

ZTEST_SUITE(shape_icons, NULL, SetUp, NULL, Clean, NULL);

ZTEST(shape_icons, test_vector_backend_respects_disabled_document_loaders) {
    // Shapes need the rasterizer and raw images. Disabled document loaders must
    // not pull their stdio/POSIX dependencies into the ESP32-S3 firmware.
    std::unique_ptr<Tvg_Paint, decltype(&tvg_paint_del)> picture(tvg_picture_new(), tvg_paint_del);
    zassert_not_null(picture.get());
#if !LV_USE_LOTTIE
    constexpr char svg[] = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"2\" height=\"2\">"
        "<rect width=\"2\" height=\"2\"/></svg>";
    zassert_equal(
        tvg_picture_load_data(picture.get(), svg, sizeof(svg) - 1, "svg", true),
        TVG_RESULT_NOT_SUPPORTED);
    zassert_is_null(tvg_lottie_animation_new());
#endif
    uint32_t pixel = 0xFFFFFFFF;
    zassert_equal(tvg_picture_load_raw(picture.get(), &pixel, 1, 1, true), TVG_RESULT_SUCCESS);
}

ZTEST(shape_icons, test_factory_registers_all_shapes_and_widgets_report_their_properties) {
    auto types = IconFactory::GetInstance().GetAvailableTypes();
    for(auto type : shape_types) {
        zassert_true(std::find(types.begin(), types.end(), type) != types.end());
        auto widget = MakeWidget(Configuration(type));
        zassert_equal(Mask(*widget).header.w, 64);
        zassert_equal(Mask(*widget).header.h, 48);
        auto supported = widget->GetSupportedProperties();
        for(auto property : { WidgetPropertyType::WIDTH_PX, WidgetPropertyType::HEIGHT_PX,
                              WidgetPropertyType::STROKE_PX })
            zassert_true(std::find(supported.begin(), supported.end(), property) != supported.end());
        auto supports = [&](WidgetPropertyType property) {
            return std::find(supported.begin(), supported.end(), property) != supported.end();
        };
        zassert_equal(supports(WidgetPropertyType::FILL_MODE), type != IconType::Line);
        zassert_equal(supports(WidgetPropertyType::DIRECTION), type == IconType::Line);
        zassert_equal(supports(WidgetPropertyType::CORNER_RAD_PX), type != IconType::Line && type != IconType::Oval);
        zassert_false(supports(WidgetPropertyType::LABEL));
        zassert_false(supports(WidgetPropertyType::FILE_PATH));
        zassert_false(supports(WidgetPropertyType::IMG_WIDTH));
    }
}

ZTEST(shape_icons, test_icon_metadata_is_specific_before_rendering) {
    auto root = MakeRoot();
    auto before = lv_obj_get_child_count(root->GetObject());
    for(auto type : IconFactory::GetInstance().GetAvailableTypes()) {
        WidgetPropertyStore store;
        IconFactory::GetInstance().RegisterProperties(type, store);
        zassert_equal(lv_obj_get_child_count(root->GetObject()), before);
        zassert_equal(store.IsRegistered(WidgetPropertyType::LABEL), type == IconType::Label);
        zassert_equal(store.IsRegistered(WidgetPropertyType::FILE_PATH), type == IconType::Image);
        zassert_equal(store.IsRegistered(WidgetPropertyType::WIDTH_PX),
            std::find(shape_types.begin(), shape_types.end(), type) != shape_types.end());
    }
    IconWidget line(1, root, WidgetContext {}, IconType::Line);
    auto line_properties = line.GetSupportedProperties();
    zassert_true(std::find(line_properties.begin(), line_properties.end(), WidgetPropertyType::DIRECTION)
        != line_properties.end());
    zassert_true(std::find(line_properties.begin(), line_properties.end(), WidgetPropertyType::FILL_MODE)
        == line_properties.end());
}

ZTEST(shape_icons, test_irrelevant_bindings_do_not_reconfigure_a_shape) {
    auto configuration = Configuration(IconType::Oval);
    Bind(*configuration, WidgetPropertyType::CORNER_RAD_PX);
    Bind(*configuration, WidgetPropertyType::DIRECTION);
    auto widget = MakeWidget(configuration);
    auto before = Pixels(*widget);
    const auto* data = Mask(*widget).data;
    Publish(12);
    zassert_equal(Mask(*widget).data, data);
    zassert_true(Pixels(*widget) == before);
    auto configuration_for_line = Configuration(IconType::Line);
    Bind(*configuration_for_line, WidgetPropertyType::FILL_MODE);
    Bind(*configuration_for_line, WidgetPropertyType::CORNER_RAD_PX);
    auto line = MakeWidget(configuration_for_line);
    auto line_before = Pixels(*line);
    Publish(1);
    zassert_true(Pixels(*line) == line_before);
}

ZTEST(shape_icons, test_default_shapes_have_visible_geometry) {
    for(auto type : shape_types) {
        auto configuration = Configuration(type);
        configuration->properties.clear();
        configuration->properties[WidgetPropertyType::ICON_TYPE] = static_cast<int>(type);
        auto widget = MakeWidget(configuration);
        zassert_equal(Mask(*widget).header.w, 32);
        zassert_equal(Mask(*widget).header.h, 32);
        zassert_within(Alpha(*widget, 16, 24), type == IconType::Line ? 0 : 255, 1,
            "Type %d: alpha %u", static_cast<int>(type), Alpha(*widget, 16, 24));
    }
}

ZTEST(shape_icons, test_rectangle_fill_outline_and_rounding_cross_tile_boundaries) {
    auto filled = MakeWidget(Configuration(IconType::Rectangle, 97, 65));
    for(int y = 0; y < 65; ++y)
        for(int x = 0; x < 97; ++x)
            zassert_within(Alpha(*filled, x, y), 255, 1, "A seam at %d,%d: alpha %u", x, y, Alpha(*filled, x, y));

    auto outline = MakeWidget(Configuration(IconType::Rectangle, 64, 48, WidgetFillMode::Outline, 12, 4));
    zassert_equal(Alpha(*outline, 0, 0), 0);
    zassert_within(Alpha(*outline, 32, 1), 255, 1);
    zassert_equal(Alpha(*outline, 32, 6), 0);
    zassert_equal(Alpha(*outline, 32, 24), 0);

    auto rounded = MakeWidget(Configuration(IconType::Rectangle, 64, 48, WidgetFillMode::Filled, 12));
    zassert_equal(Alpha(*rounded, 0, 0), 0);
    zassert_within(Alpha(*rounded, 32, 24), 255, 1);
}

ZTEST(shape_icons, test_triangles_have_distinct_geometry_and_rounded_outline_corners) {
    auto isosceles = MakeWidget(Configuration(IconType::TriangleIsosceles));
    auto right = MakeWidget(Configuration(IconType::TriangleRight));
    zassert_equal(Alpha(*isosceles, 1, 8), 0);
    zassert_within(Alpha(*right, 1, 8), 255, 1);
    for(int y = 0; y < 48; ++y)
        for(int x = 0; x < 64; ++x)
            if(static_cast<float>(y + 1) < static_cast<float>(x) * 0.75f)
                zassert_equal(Alpha(*right, x, y), 0, "Stray pixel outside the triangle at %d,%d", x, y);
    for(auto type : { IconType::TriangleIsosceles, IconType::TriangleRight }) {
        auto filled = MakeWidget(Configuration(type, 64, 48, WidgetFillMode::Filled, 10));
        auto outline = MakeWidget(Configuration(type, 64, 48, WidgetFillMode::Outline, 10, 3));
        zassert_equal(Alpha(*filled, 1, 46), 0);
        zassert_equal(Alpha(*outline, 1, 46), 0);
        zassert_within(Alpha(*filled, 24, 32), 255, 1);
        zassert_equal(Alpha(*outline, 24, 32), 0);
        zassert_within(Alpha(*outline, 32, 46), 255, 1);
    }
}

ZTEST(shape_icons, test_polygon_base_supports_rounded_pentagons_in_both_windings) {
    std::vector<lv_fpoint_t> vertices { { 32, 0 }, { 64, 24 }, { 52, 64 }, { 12, 64 }, { 0, 24 } };
    for(auto fill : { WidgetFillMode::Filled, WidgetFillMode::Outline }) {
        for(int radius : { 0, 8, 999 }) {
            auto pixels = PolygonPixels(vertices, fill, radius);
            auto reversed = vertices;
            std::reverse(reversed.begin(), reversed.end());
            zassert_true(pixels == PolygonPixels(reversed, fill, radius));
            zassert_equal(pixels[0], 0);
            zassert_within(pixels[32 * 64 + 32], fill == WidgetFillMode::Filled ? 255 : 0, 1);
            zassert_within(pixels[62 * 64 + 32], 255, 1);
        }
    }
    auto sharp = PolygonPixels(vertices);
    auto rounded = PolygonPixels(vertices, WidgetFillMode::Filled, 8);
    zassert_true(sharp[1 * 64 + 32] > 0);
    zassert_equal(rounded[1 * 64 + 32], 0);
}

ZTEST(shape_icons, test_polygon_outline_keeps_uniform_inset_when_a_short_edge_disappears) {
    // The diagonal cut at the top right disappears from the inner contour at
    // this stroke width. The remaining hole is the square [16,48] x [16,48].
    std::vector<lv_fpoint_t> vertices { { 0, 0 }, { 56, 0 }, { 64, 8 }, { 64, 64 }, { 0, 64 } };
    auto pixels = PolygonPixels(vertices, WidgetFillMode::Outline, 0, 16);
    for(int y = 12; y < 52; ++y) {
        for(int x = 12; x < 52; ++x) {
            bool hole = x >= 16 && x < 48 && y >= 16 && y < 48;
            zassert_within(pixels[y * 64 + x], hole ? 0 : 255, 1, "Unexpected inset at %d,%d", x, y);
        }
    }
    auto filled = PolygonPixels(vertices);
    for(int stroke : { 32, 999 })
        zassert_true(filled == PolygonPixels(vertices, WidgetFillMode::Outline, 0, stroke));
}

ZTEST(shape_icons, test_polygon_base_rejects_degenerate_and_nonconvex_geometry) {
    for(const std::vector<lv_fpoint_t>& vertices : {
            std::vector<lv_fpoint_t> {},
            { { 0, 0 }, { 64, 0 } },
            { { 0, 0 }, { 32, 32 }, { 64, 64 } },
            { { 0, 0 }, { 64, 0 }, { 32, 16 }, { 64, 64 }, { 0, 64 } },
            { { 0, 0 }, { 64, 64 }, { 0, 64 }, { 64, 0 } } }) {
        auto pixels = PolygonPixels(vertices, WidgetFillMode::Outline, 8);
        zassert_true(std::all_of(pixels.begin(), pixels.end(), [](uint8_t alpha) { return alpha == 0; }));
    }
}

ZTEST(shape_icons, test_oval_is_an_ellipse_with_a_transparent_outline_interior) {
    auto filled = MakeWidget(Configuration(IconType::Oval, 96, 48));
    zassert_equal(Alpha(*filled, 24, 1), 0); // A capsule would be filled here.
    zassert_within(Alpha(*filled, 48, 1), 255, 1);
    zassert_within(Alpha(*filled, 1, 24), 255, 1);
    auto outline = MakeWidget(Configuration(IconType::Oval, 96, 48, WidgetFillMode::Outline));
    zassert_within(Alpha(*outline, 48, 1), 255, 1);
    zassert_equal(Alpha(*outline, 48, 24), 0);
    auto radius_ignored = MakeWidget(Configuration(IconType::Oval, 96, 48, WidgetFillMode::Outline, 999));
    zassert_true(Pixels(*outline) == Pixels(*radius_ignored));
}

ZTEST(shape_icons, test_line_direction_controls_orientation_and_reversal_preserves_appearance) {
    auto left = MakeWidget(Configuration(IconType::Line));
    zassert_within(Alpha(*left, 1, 24), 255, 1);
    zassert_equal(Alpha(*left, 32, 1), 0);
    auto right = MakeWidget(Configuration(IconType::Line, 64, 48, WidgetFillMode::Outline, 20, 4,
        WidgetDirection::RightToLeft));
    zassert_true(Pixels(*left) == Pixels(*right));
    auto down = MakeWidget(Configuration(IconType::Line, 64, 48, WidgetFillMode::Filled, 0, 4,
        WidgetDirection::TopToBottom));
    zassert_within(Alpha(*down, 32, 1), 255, 1);
    zassert_equal(Alpha(*down, 1, 24), 0);
    auto up = MakeWidget(Configuration(IconType::Line, 64, 48, WidgetFillMode::Filled, 0, 4,
        WidgetDirection::BottomToTop));
    zassert_true(Pixels(*up) == Pixels(*down));
}

ZTEST(shape_icons, test_lines_allow_zero_unused_dimension_and_keep_the_entire_stroke) {
    auto horizontal = MakeWidget(Configuration(IconType::Line, 64, 0, WidgetFillMode::Filled, 0, 7));
    zassert_equal(Mask(*horizontal).header.h, 7);
    zassert_within(Alpha(*horizontal, 32, 0), 255, 1);
    zassert_within(Alpha(*horizontal, 32, 6), 255, 1);
    auto vertical = MakeWidget(Configuration(IconType::Line, 0, 48, WidgetFillMode::Filled, 0, 7,
        WidgetDirection::TopToBottom));
    zassert_equal(Mask(*vertical).header.w, 7);
    zassert_within(Alpha(*vertical, 0, 24), 255, 1);
    zassert_within(Alpha(*vertical, 6, 24), 255, 1);
}

ZTEST(shape_icons, test_invalid_sizes_and_zero_strokes_suppress_drawing_safely) {
    for(double width : { -1.0, 0.0, 1e100, std::numeric_limits<double>::infinity() }) {
        auto configuration = Configuration(IconType::Rectangle);
        configuration->properties[WidgetPropertyType::WIDTH_PX] = width;
        auto widget = MakeWidget(configuration);
        zassert_is_null(lv_image_get_src(ImageObject(*widget)));
        zassert_equal(lv_obj_get_style_opa(ImageObject(*widget), LV_PART_MAIN), LV_OPA_TRANSP);
    }
    auto too_large = MakeWidget(Configuration(IconType::Oval, 5000, 5000));
    zassert_is_null(lv_image_get_src(ImageObject(*too_large)));
    for(auto type : shape_types) {
        auto widget = MakeWidget(Configuration(type, 64, 48, WidgetFillMode::Outline, 0, 0));
        zassert_is_null(lv_image_get_src(ImageObject(*widget)));
    }
}

ZTEST(shape_icons, test_large_radii_and_thicknesses_fit_small_shapes) {
    for(auto type : { IconType::Rectangle, IconType::TriangleIsosceles, IconType::TriangleRight, IconType::Oval }) {
        auto widget = MakeWidget(Configuration(type, 8, 8, WidgetFillMode::Outline, 999, 999));
        auto pixels = Pixels(*widget);
        zassert_true(std::any_of(pixels.begin(), pixels.end(), [](uint8_t alpha) { return alpha > 0; }));
    }
    auto pixel = MakeWidget(Configuration(IconType::Rectangle, 1, 1));
    zassert_within(Alpha(*pixel, 0, 0), 255, 1);
}

ZTEST(shape_icons, test_bound_size_tracks_hidden_updates_and_replays_visible_geometry) {
    auto configuration = Configuration(IconType::Rectangle);
    Bind(*configuration, WidgetPropertyType::WIDTH_PX);
    auto widget = MakeWidget(configuration);
    Publish(96);
    zassert_equal(Mask(*widget).header.w, 96);
    zassert_within(Alpha(*widget, 95, 24), 255, 1);
    widget->OnDeactivated();
    Publish(80);
    zassert_equal(Mask(*widget).header.w, 96);
    widget->OnActivated();
    zassert_equal(Mask(*widget).header.w, 80);
}

ZTEST(shape_icons, test_async_binding_rasterizes_on_the_event_bus_worker) {
    auto configuration = Configuration(IconType::TriangleIsosceles, 64, 48, WidgetFillMode::Outline, 10);
    Bind(*configuration, WidgetPropertyType::WIDTH_PX);
    auto widget = MakeWidget(configuration);
    SensorEventsChannel::GetInstance().PublishAsync({
        .source_id = 0,
        .type = SensorEventType::DataUpdated,
        .payload = {{ SensorPayloadType::Value, 96.0F }}
    });
    int64_t deadline = k_uptime_get() + 1000;
    bool updated = false;
    while(!updated && k_uptime_get() < deadline) {
        {
            ScopedLvglLock lock;
            updated = Mask(*widget).header.w == 96;
        }
        if(!updated)
            k_msleep(1);
    }
    zassert_true(updated);
}

ZTEST(shape_icons, test_overlay_redraw_preserves_the_underlying_shape) {
    for(auto type : shape_types) {
        auto widget = MakeWidget(Configuration(type, 160, 96, WidgetFillMode::Outline, 10, 6));
        auto before = Pixels(*widget);
        eerie_leap::views::OverlayHost host(nullptr);
        zassert_equal(host.Render(), 0);
        for(uint32_t i = 0; i < 3; ++i) {
            auto group = host.CreateGroup(i);
            group->AddScreen(std::make_shared<views_test::FakeScreen>(i, i, 0, true, group->GetContainer()));
            zassert_equal(host.Push(std::move(group)), 0);
            lv_refr_now(nullptr);
            zassert_equal(host.GetDepth(), 1);
            zassert_true(Pixels(*widget) == before);
            zassert_equal(host.Pop(), 0);
            lv_refr_now(nullptr);
            zassert_true(Pixels(*widget) == before);
        }
    }
}

ZTEST(shape_icons, test_bound_fill_radius_thickness_and_direction_refresh_the_mask) {
    for(auto property : { WidgetPropertyType::FILL_MODE, WidgetPropertyType::CORNER_RAD_PX,
                         WidgetPropertyType::STROKE_PX, WidgetPropertyType::DIRECTION }) {
        auto type = property == WidgetPropertyType::DIRECTION ? IconType::Line : IconType::Rectangle;
        auto configuration = Configuration(type, 64, 48,
            property == WidgetPropertyType::STROKE_PX ? WidgetFillMode::Outline : WidgetFillMode::Filled);
        Bind(*configuration, property);
        auto widget = MakeWidget(configuration);
        auto before = Pixels(*widget);
        Publish(property == WidgetPropertyType::FILL_MODE ? 1 : property == WidgetPropertyType::DIRECTION ? 3 : 12);
        zassert_true(before != Pixels(*widget), "Property %d did not update drawing", static_cast<int>(property));
    }
}

ZTEST(shape_icons, test_arc_repositions_when_shape_height_changes) {
    auto configuration = Configuration(IconType::Rectangle, 32, 32);
    configuration->properties[WidgetPropertyType::POSITION_ANGLE] = 0;
    Bind(*configuration, WidgetPropertyType::HEIGHT_PX);
    auto widget = std::make_unique<ArcIconWidget>(1, MakeRoot(), WidgetContext {});
    widget->Configure(configuration);
    zassert_equal(widget->Render(), 0);
    widget->OnActivated();
    int before = lv_obj_get_y(ImageObject(*widget));
    Publish(64);
    zassert_equal(Mask(*widget).header.h, 64);
    zassert_equal(std::abs(lv_obj_get_y(ImageObject(*widget)) - before), 16);
    zassert_equal(lv_obj_get_style_transform_pivot_y(ImageObject(*widget), LV_PART_MAIN), 32);
}

ZTEST(shape_icons, test_theme_and_active_state_repaint_without_rebuilding_geometry) {
    class BlueTheme : public DefaultTheme {
        LvglColor GetAccentColor() const override { return LvglColor(0x3366FF, 128); }
    };
    auto widget = MakeWidget(Configuration(IconType::Rectangle, 32, 32));
    const auto* data = Mask(*widget).data;
    ThemeManager::GetInstance().SetTheme(std::make_shared<BlueTheme>());
    zassert_equal(Mask(*widget).data, data);
    zassert_equal(lv_obj_get_style_opa(ImageObject(*widget), LV_PART_MAIN), 128);
    zassert_equal(lv_color_to_u32(lv_obj_get_style_image_recolor(ImageObject(*widget), LV_PART_MAIN)),
        lv_color_to_u32(lv_color_hex(0x3366FF)));
    widget->SetIsActive(false);
    zassert_equal(lv_obj_get_style_opa(ImageObject(*widget), LV_PART_MAIN), 0);
    widget->SetIsActive(true);
    zassert_equal(lv_obj_get_style_opa(ImageObject(*widget), LV_PART_MAIN), 128);
}

ZTEST(shape_icons, test_rendered_gallery_preserves_background_and_recolors_masks) {
    constexpr std::array names { "rectangle", "triangle_isosceles", "triangle_right", "oval", "line" };
    for(size_t i = 0; i < shape_types.size(); ++i) {
        for(int variant = 0; variant < 4; ++variant) {
            auto widget = MakeWidget(Configuration(shape_types[i], 64, 48,
                variant % 2 == 0 ? WidgetFillMode::Filled : WidgetFillMode::Outline,
                variant >= 2 ? 12 : 0, 4,
                variant >= 2 ? WidgetDirection::TopToBottom : WidgetDirection::LeftToRight));
            auto root = lv_obj_get_parent(widget->GetContainer()->GetObject());
            lv_obj_set_size(root, 96, 72);
            lv_obj_set_style_bg_color(root, lv_color_hex(0xF3F4F6), 0);
            lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
            lv_obj_update_layout(root);
            std::unique_ptr<lv_draw_buf_t, decltype(&lv_draw_buf_destroy)> snapshot(
                lv_snapshot_take(root, LV_COLOR_FORMAT_ARGB8888), lv_draw_buf_destroy);
            zassert_not_null(snapshot);
            zassert_equal(snapshot->header.w, 96);
            zassert_equal(snapshot->header.h, 72);
            size_t colored_pixels = 0;
            for(int y = 0; y < 72; ++y) {
                auto pixels = reinterpret_cast<const lv_color32_t*>(snapshot->data + y * snapshot->header.stride);
                for(int x = 0; x < 96; ++x) {
                    bool background = pixels[x].red == 0xF3 && pixels[x].green == 0xF4 && pixels[x].blue == 0xF6;
                    if(x < 16 || x >= 80 || y < 12 || y >= 60)
                        zassert_true(background, "Shape %s escaped its bounds at %d,%d", names[i], x, y);
                    if(pixels[x].red > 200 && pixels[x].green < 150 && pixels[x].blue < 150)
                        ++colored_pixels;
                    if(variant % 2 == 1 && shape_types[i] != IconType::Line && x == 40 && y == 44)
                        zassert_true(background, "Outline interior must preserve the background");
                }
            }
            zassert_true(colored_pixels > 20, "Shape %s did not render in the accent color", names[i]);
            SavePreview(*snapshot, names[i], variant);
        }
    }
}

ZTEST(shape_icons, test_repeated_render_and_destruction_leave_no_duplicate_owners) {
    auto root = MakeRoot();
    for(auto type : shape_types) {
        {
            IconWidget widget(1, root, WidgetContext {});
            widget.Configure(Configuration(type));
            zassert_equal(widget.Render(), 0);
            zassert_equal(widget.Render(), 0);
            zassert_not_null(lv_image_get_src(ImageObject(widget)));
        }
        zassert_equal(lv_obj_get_child_count(root->GetObject()), 0);
    }
}

ZTEST(shape_icons, test_widget_direction_preserves_bar_orientation_and_ranges) {
    auto root = MakeRoot();
    for(auto direction : { WidgetDirection::LeftToRight, WidgetDirection::RightToLeft,
                          WidgetDirection::TopToBottom, WidgetDirection::BottomToTop }) {
        auto bar = Frame::Create(lv_bar_create(root->GetObject())).Build();
        BarIndicator::UpdateDirection(bar.GetObject(), direction, 0, 100);
        bool horizontal = direction == WidgetDirection::LeftToRight || direction == WidgetDirection::RightToLeft;
        zassert_equal(lv_bar_get_orientation(bar.GetObject()),
            horizontal ? LV_BAR_ORIENTATION_HORIZONTAL : LV_BAR_ORIENTATION_VERTICAL);
        zassert_equal(lv_bar_get_min_value(bar.GetObject()), direction == WidgetDirection::TopToBottom ? 100 : 0);
        zassert_equal(lv_bar_get_max_value(bar.GetObject()), direction == WidgetDirection::TopToBottom ? 0 : 100);
    }
}
