#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string_view>

#include <lvgl.h>
#include <lvgl_mem.h>
#include <zephyr/ztest.h>

#include "domain/ui_domain/lvgl_lock.h"
#include "domain/ui_domain/models/widget_configuration.h"
#include "event_bus/event_channels.h"
#include "subsys/assets/asset_kind.h"
#include "views/themes/theme_manager.h"
#include "views/utilities/font_cache.h"
#include "views/widgets/widget_factory.h"

#include "views_test_support.h"

using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::widgets;
using eerie_leap::domain::ui_domain::ScopedLvglLock;
using eerie_leap::subsys::assets::AssetKind;
using eerie_leap::views::themes::ThemeManager;
using eerie_leap::views::utilities::FontCache;
using eerie_leap::views::utilitites::Frame;

namespace {

// The font fixture, embedded at build time (see this suite's CMakeLists.txt).
const uint8_t TEST_FONT_TTF[] = {
#include "test_font_ttf.inc"
};

WidgetContext MakeContext() {
    auto store = views_test::MakeAssetStore("ui_assets/fonts");
    zassert_true(store->Save("test.ttf", TEST_FONT_TTF));
    auto assets = views_test::MakeAssetsManager(AssetKind::FONTS, store);
    return WidgetContext{ .assets_manager = assets, .font_cache = std::make_shared<FontCache>(assets) };
}

std::shared_ptr<WidgetConfiguration> Configuration(WidgetType type, std::string_view file = {}, int size_px = 0) {
    auto configuration = std::make_shared<WidgetConfiguration>(std::allocator_arg, std::pmr::get_default_resource());
    configuration->id = 1;
    configuration->type = type;
    if(type == WidgetType::BasicIcon) {
        configuration->properties[WidgetPropertyType::ICON_TYPE] = static_cast<int>(IconType::Label);
        configuration->properties[WidgetPropertyType::LABEL] = std::pmr::string("Av");
    }
    if(!file.empty())
        configuration->properties[WidgetPropertyType::FONT_FILE_PATH] = std::pmr::string(file);
    if(size_px > 0)
        configuration->properties[WidgetPropertyType::FONT_SIZE_PX] = size_px;
    return configuration;
}

std::unique_ptr<IWidget> Render(
    const std::shared_ptr<WidgetConfiguration>& configuration,
    const WidgetContext& context,
    const std::shared_ptr<Frame>& root
) {
    auto widget = WidgetFactory::GetInstance().CreateWidget(configuration, root, context);
    zassert_equal(widget->Render(), 0);
    return widget;
}

std::shared_ptr<Frame> MakeRoot() {
    return std::make_shared<Frame>(Frame::CreateWrapped().SetWidth(100, false).SetHeight(100, false).Build());
}

lv_obj_t* FindLabel(lv_obj_t* object) {
    lv_obj_t* label = nullptr;
    lv_obj_tree_walk(
        object,
        [](lv_obj_t* current, void* result) {
            if(!lv_obj_check_type(current, &lv_label_class))
                return LV_OBJ_TREE_WALK_NEXT;

            *static_cast<lv_obj_t**>(result) = current;
            return LV_OBJ_TREE_WALK_END;
        },
        static_cast<void*>(&label)
    );
    return label;
}

const lv_font_t* FontOf(const IWidget& widget) {
    auto* label = FindLabel(views_test::WidgetContent(widget));
    zassert_not_null(label);
    return lv_obj_get_style_text_font(label, LV_PART_MAIN);
}

size_t LvglHeapBytes() {
    sys_memory_stats stats{};
    lvgl_heap_stats(&stats);
    return stats.allocated_bytes;
}

void* Setup() {
    views_test::EnsureTestDisplay();
    eerie_leap::event_bus::InitializeEventChannels();
    return nullptr;
}

} // namespace

ZTEST_SUITE(text_fonts, NULL, Setup, NULL, views_test::CleanTestDisplay, NULL);

ZTEST(text_fonts, test_text_widgets_draw_with_the_configured_font) {
    const ScopedLvglLock lock;
    const auto context = MakeContext();
    const auto& theme = ThemeManager::GetInstance().GetCurrentTheme();

    for(auto type : { WidgetType::IndicatorDigital, WidgetType::BasicIcon }) {
        const auto theme_font = type == WidgetType::BasicIcon ? theme.GetPrimaryFont() : theme.GetPrimaryFontLarge();
        auto small = Render(Configuration(type, "test.ttf", 24), context, MakeRoot());
        auto large = Render(Configuration(type, "test.ttf", 48), context, MakeRoot());

        zassert_not_equal(FontOf(*large), theme_font.ToLvFont(), "widget %d", static_cast<int>(type));
        zassert_true(
            lv_font_get_line_height(FontOf(*large)) > lv_font_get_line_height(FontOf(*small)), "widget %d",
            static_cast<int>(type)
        );
    }
}

ZTEST(text_fonts, test_without_a_font_file_the_theme_font_is_used) {
    const ScopedLvglLock lock;
    const auto context = MakeContext();
    const auto& theme = ThemeManager::GetInstance().GetCurrentTheme();

    auto digital = Render(Configuration(WidgetType::IndicatorDigital), context, MakeRoot());
    auto icon = Render(Configuration(WidgetType::BasicIcon), context, MakeRoot());

    zassert_equal(FontOf(*digital), theme.GetPrimaryFontLarge().ToLvFont());
    zassert_equal(FontOf(*icon), theme.GetPrimaryFont().ToLvFont());
}

ZTEST(text_fonts, test_a_missing_font_file_falls_back_to_the_theme_font) {
    const ScopedLvglLock lock;
    const auto context = MakeContext();

    auto widget = Render(Configuration(WidgetType::IndicatorDigital, "missing.ttf", 48), context, MakeRoot());

    zassert_equal(FontOf(*widget), ThemeManager::GetInstance().GetCurrentTheme().GetPrimaryFontLarge().ToLvFont());
}

ZTEST(text_fonts, test_widgets_with_the_same_file_and_size_share_one_font) {
    const ScopedLvglLock lock;
    const auto context = MakeContext();

    auto first = Render(Configuration(WidgetType::IndicatorDigital, "test.ttf", 48), context, MakeRoot());
    auto second = Render(Configuration(WidgetType::IndicatorDigital, "test.ttf", 48), context, MakeRoot());

    zassert_equal(FontOf(*first), FontOf(*second));
}

ZTEST(text_fonts, test_destroying_the_widgets_returns_the_memory) {
    const ScopedLvglLock lock;

    // LVGL's own caches fill on the first draw; start from there.
    {
        auto warm = Render(Configuration(WidgetType::IndicatorDigital), WidgetContext{}, MakeRoot());
        lv_refr_now(nullptr);
    }
    lv_obj_clean(lv_screen_active());
    const size_t baseline = LvglHeapBytes();

    {
        const auto context = MakeContext();
        auto first = Render(Configuration(WidgetType::IndicatorDigital, "test.ttf", 48), context, MakeRoot());
        auto second = Render(Configuration(WidgetType::BasicIcon, "test.ttf", 32), context, MakeRoot());
        lv_refr_now(nullptr);
        zassert_true(LvglHeapBytes() > baseline);
    }
    lv_obj_clean(lv_screen_active());

    zassert_equal(LvglHeapBytes(), baseline);
}
