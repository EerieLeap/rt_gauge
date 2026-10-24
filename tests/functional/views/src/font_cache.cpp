#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

#include <lvgl.h>
#include <lvgl_mem.h>
#include <zephyr/ztest.h>

#include "domain/ui_domain/lvgl_lock.h"
#include "subsys/assets/asset_kind.h"
#include "subsys/assets/asset_provider.h"
#include "subsys/assets/assets_manager.h"
#include "views/assets/fonts/builtin_fonts.h"
#include "views/utilities/font_cache.h"

#include "views_test_support.h"

using eerie_leap::domain::ui_domain::ScopedLvglLock;
using eerie_leap::subsys::assets::AssetKind;
using eerie_leap::subsys::assets::BuiltInAsset;
using eerie_leap::views::assets::BuiltInFonts;
using eerie_leap::views::utilities::FontCache;
using eerie_leap::views::utilities::LvglFont;

namespace {

// The font fixture, embedded at build time (see this suite's CMakeLists.txt).
const uint8_t TEST_FONT_TTF[] = {
#include "test_font_ttf.inc"
};

constexpr uint32_t CJK_CHARACTER = 0x4E2D; // the test font has Latin letters only

std::shared_ptr<eerie_leap::subsys::assets::AssetStore> FontStore() {
    auto store = views_test::MakeAssetStore("ui_assets/fonts");
    zassert_true(store->Save("test.ttf", TEST_FONT_TTF));
    zassert_true(store->Save("bad.ttf", views_test::TextBytes("this is not a font, but it is long enough to read")));
    return store;
}

std::shared_ptr<FontCache> MakeCache() {
    return std::make_shared<FontCache>(views_test::MakeAssetsManager(AssetKind::FONTS, FontStore()));
}

bool HasGlyph(const lv_font_t* font, uint32_t character) {
    lv_font_glyph_dsc_t glyph;
    return lv_font_get_glyph_dsc(font, &glyph, character, 0);
}

lv_obj_t* DrawLabel(const LvglFont& font) {
    auto* label = lv_label_create(lv_screen_active());
    lv_obj_set_style_text_font(label, font.ToLvFont(), LV_PART_MAIN);
    font.BindTo(label);
    lv_label_set_text(label, "Aa Gg");
    lv_refr_now(nullptr);
    return label;
}

size_t LvglHeapBytes() {
    sys_memory_stats stats{};
    lvgl_heap_stats(&stats);
    return stats.allocated_bytes;
}

void* Setup() {
    views_test::EnsureTestDisplay();
    return nullptr;
}

} // namespace

ZTEST_SUITE(font_cache, NULL, Setup, NULL, views_test::CleanTestDisplay, NULL);

ZTEST(font_cache, test_a_font_loads_at_its_size) {
    const ScopedLvglLock lock;
    auto cache = MakeCache();

    auto font = cache->Acquire("test.ttf", 24);
    zassert_true(font.has_value());
    const int32_t line_height = lv_font_get_line_height(font->ToLvFont());
    zassert_true(line_height >= 24 && line_height <= 48, "line height %d", line_height);
    zassert_true(HasGlyph(font->ToLvFont(), 'A'));
    zassert_false(HasGlyph(font->ToLvFont(), CJK_CHARACTER));
}

ZTEST(font_cache, test_a_name_and_size_share_one_font) {
    const ScopedLvglLock lock;
    auto cache = MakeCache();

    auto first = cache->Acquire("test.ttf", 24);
    auto second = cache->Acquire("test.ttf", 24);
    auto large = cache->Acquire("test.ttf", 48);
    zassert_true(first && second && large);
    zassert_equal(first->ToLvFont(), second->ToLvFont());
    zassert_not_equal(first->ToLvFont(), large->ToLvFont());
    zassert_true(lv_font_get_line_height(large->ToLvFont()) > lv_font_get_line_height(first->ToLvFont()));
}

ZTEST(font_cache, test_what_cannot_load_gives_nothing) {
    const ScopedLvglLock lock;
    auto cache = MakeCache();

    zassert_false(cache->Acquire("missing.ttf", 24).has_value());
    zassert_false(cache->Acquire("bad.ttf", 24).has_value());
    zassert_false(cache->Acquire("test.ttf", 7).has_value());
    zassert_false(cache->Acquire("test.ttf", 257).has_value());
    zassert_false(cache->Acquire("", 24).has_value());
}

ZTEST(font_cache, test_a_unit_without_fonts_gives_nothing) {
    const ScopedLvglLock lock;

    FontCache images_only(views_test::MakeAssetsManager(AssetKind::IMAGES, FontStore()));
    zassert_false(images_only.Acquire("test.ttf", 24).has_value());

    FontCache no_manager(nullptr);
    zassert_false(no_manager.Acquire("test.ttf", 24).has_value());
}

ZTEST(font_cache, test_an_upload_changes_what_the_next_acquire_loads) {
    const ScopedLvglLock lock;
    auto store = FontStore();
    FontCache cache(views_test::MakeAssetsManager(AssetKind::FONTS, store));

    auto before = cache.Acquire("test.ttf", 24);
    zassert_true(before.has_value());
    const lv_font_t* before_font = before->ToLvFont();
    const int32_t before_height = lv_font_get_line_height(before_font);

    zassert_true(store->Save("test.ttf", TEST_FONT_TTF));
    auto after = cache.Acquire("test.ttf", 24);
    zassert_true(after.has_value());
    zassert_not_equal(after->ToLvFont(), before_font);

    zassert_equal(before->ToLvFont(), before_font);
    zassert_equal(lv_font_get_line_height(before_font), before_height);
    zassert_true(HasGlyph(before_font, 'A'));
}

ZTEST(font_cache, test_deleting_the_labels_returns_the_memory) {
    const ScopedLvglLock lock;
    auto cache = MakeCache();

    // LVGL's own caches fill on the first draw; start from there.
    lv_obj_delete(DrawLabel(LvglFont{ &lv_font_montserrat_20 }));
    const size_t baseline = LvglHeapBytes();

    auto small = cache->Acquire("test.ttf", 24);
    auto large = cache->Acquire("test.ttf", 48);
    zassert_true(small && large);
    auto* small_label = DrawLabel(*small);
    auto* large_label = DrawLabel(*large);
    zassert_true(LvglHeapBytes() > baseline);

    lv_obj_delete(small_label);
    lv_obj_delete(large_label);
    small.reset();
    large.reset();
    zassert_equal(LvglHeapBytes(), baseline);
}

#ifdef CONFIG_EERIE_LEAP_UI_BUILTIN_FONTS

namespace {

constexpr int32_t BUILT_IN_SIZE_PX = 40;
// What a client may upload as a font (EERIE_LEAP_UI_FONT_MAX_SIZE): a built-in must not be larger.
constexpr size_t FONT_UPLOAD_LIMIT = 256 * 1024;
constexpr uint32_t E_ACUTE = 0x00E9;
constexpr uint32_t NO_BREAK_SPACE = 0x00A0;
// A TrueType file starts with this version.
constexpr std::array<uint8_t, 4> TRUETYPE_VERSION = { 0x00, 0x01, 0x00, 0x00 };

// A registry as the unit has it: the built-in fonts, and a store that holds no font at all.
std::shared_ptr<eerie_leap::subsys::assets::AssetsManager> BuiltInFontsManager(
    std::shared_ptr<eerie_leap::subsys::assets::AssetStore> store = views_test::MakeAssetStore("ui_assets/no_fonts")
) {
    return views_test::MakeAssetsManager(AssetKind::FONTS, std::move(store), BuiltInFonts());
}

} // namespace

ZTEST(font_cache, test_the_built_in_fonts_are_the_three_latin_1_subsets) {
    const auto built_ins = BuiltInFonts();

    zassert_equal(built_ins.size(), 3U);
    zassert_true(built_ins[0].name == "Rubik-Medium.ttf");
    zassert_true(built_ins[1].name == "Montserrat-Medium.ttf");
    zassert_true(built_ins[2].name == "Inconsolata-Bold.ttf");

    for(const BuiltInAsset& built_in : built_ins) {
        zassert_true(
            built_in.data.size() > TRUETYPE_VERSION.size(), "%.*s", static_cast<int>(built_in.name.size()),
            built_in.name.data()
        );
        zassert_true(
            std::equal(TRUETYPE_VERSION.begin(), TRUETYPE_VERSION.end(), built_in.data.begin()),
            "%.*s is no TrueType file", static_cast<int>(built_in.name.size()), built_in.name.data()
        );
        zassert_true(
            built_in.data.size() <= FONT_UPLOAD_LIMIT, "%.*s is bigger than a font may be",
            static_cast<int>(built_in.name.size()), built_in.name.data()
        );
    }
}

ZTEST(font_cache, test_a_built_in_font_loads_from_flash_with_no_copy_and_no_store_read) {
    const ScopedLvglLock lock;
    // The store has no file of any name: whatever loads is not from it.
    auto cache = std::make_shared<FontCache>(BuiltInFontsManager());

    for(const BuiltInAsset& built_in : BuiltInFonts()) {
        auto font = cache->Acquire(built_in.name, BUILT_IN_SIZE_PX);

        zassert_true(font.has_value(), "%.*s", static_cast<int>(built_in.name.size()), built_in.name.data());
        zassert_true(HasGlyph(font->ToLvFont(), 'A'));
        zassert_true(HasGlyph(font->ToLvFont(), E_ACUTE), "Latin-1 letters are in the subset");
        zassert_true(HasGlyph(font->ToLvFont(), NO_BREAK_SPACE));
        zassert_false(HasGlyph(font->ToLvFont(), CJK_CHARACTER));
        zassert_true(lv_font_get_line_height(font->ToLvFont()) >= BUILT_IN_SIZE_PX);
        zassert_equal(cache->GetCopiedBytes(), 0U, "A built-in is read in place, not copied to RAM");

        lv_obj_delete(DrawLabel(*font));
    }
}

ZTEST(font_cache, test_a_built_in_font_at_one_size_is_shared_and_other_sizes_are_other_fonts) {
    const ScopedLvglLock lock;
    auto store = views_test::MakeAssetStore("ui_assets/no_fonts");
    auto cache = std::make_shared<FontCache>(BuiltInFontsManager(store));

    auto first = cache->Acquire("Rubik-Medium.ttf", BUILT_IN_SIZE_PX);
    auto second = cache->Acquire("rubik-medium.ttf", BUILT_IN_SIZE_PX);
    auto large = cache->Acquire("Rubik-Medium.ttf", 2 * BUILT_IN_SIZE_PX);
    zassert_true(first && second && large);
    zassert_equal(first->ToLvFont(), second->ToLvFont(), "Names compare ignoring case, as the assets group's do");
    zassert_not_equal(first->ToLvFont(), large->ToLvFont());

    // A write in the kind changes the store's generation, not the flash.
    zassert_true(store->Save("test.ttf", TEST_FONT_TTF));
    auto after = cache->Acquire("Rubik-Medium.ttf", BUILT_IN_SIZE_PX);
    zassert_true(after.has_value());
    zassert_equal(after->ToLvFont(), first->ToLvFont());
    zassert_true(store->Delete("test.ttf"));
}

ZTEST(font_cache, test_a_built_in_wins_over_a_stored_file_of_its_name) {
    const ScopedLvglLock lock;
    auto store = views_test::MakeAssetStore("ui_assets/shadowed_fonts");
    zassert_true(
        store->Save("Rubik-Medium.ttf", views_test::TextBytes("this is not a font, but it is long enough to read"))
    );
    auto cache = std::make_shared<FontCache>(BuiltInFontsManager(store));

    auto font = cache->Acquire("Rubik-Medium.ttf", BUILT_IN_SIZE_PX);

    zassert_true(font.has_value(), "The stored file is no font; the built-in is what loads");
    zassert_true(HasGlyph(font->ToLvFont(), 'A'));
    zassert_equal(cache->GetCopiedBytes(), 0U);
    zassert_true(store->Delete("Rubik-Medium.ttf"));
}

ZTEST(font_cache, test_an_uploaded_font_still_loads_beside_the_built_ins) {
    const ScopedLvglLock lock;
    auto cache = std::make_shared<FontCache>(BuiltInFontsManager(FontStore()));

    auto uploaded = cache->Acquire("test.ttf", 24);

    zassert_true(uploaded.has_value());
    zassert_equal(cache->GetCopiedBytes(), sizeof(TEST_FONT_TTF), "An uploaded file is copied to RAM, once");
    zassert_false(cache->Acquire("missing.ttf", 24).has_value());
}

ZTEST(font_cache, test_deleting_the_labels_of_a_built_in_returns_the_memory) {
    const ScopedLvglLock lock;
    auto cache = std::make_shared<FontCache>(BuiltInFontsManager());

    lv_obj_delete(DrawLabel(LvglFont{ &lv_font_montserrat_20 }));
    const size_t baseline = LvglHeapBytes();

    auto font = cache->Acquire("Montserrat-Medium.ttf", BUILT_IN_SIZE_PX);
    zassert_true(font.has_value());
    auto* label = DrawLabel(*font);
    zassert_true(LvglHeapBytes() > baseline);

    lv_obj_delete(label);
    font.reset();
    zassert_equal(LvglHeapBytes(), baseline);
}

#endif // CONFIG_EERIE_LEAP_UI_BUILTIN_FONTS
