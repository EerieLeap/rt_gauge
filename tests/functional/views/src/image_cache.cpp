#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <lvgl.h>
#include <zephyr/ztest.h>

#include "domain/ui_domain/lvgl_lock.h"
#include "subsys/assets/asset_kind.h"
#include "subsys/assets/assets_manager.h"
#include "views/utilities/image_cache.h"

#include "image_file_fixtures.h"
#include "views_test_support.h"

using eerie_leap::domain::ui_domain::ScopedLvglLock;
using eerie_leap::subsys::assets::AssetKind;
using eerie_leap::views::utilities::ImageCache;
using eerie_leap::views::utilities::LoadedImage;

namespace {

constexpr size_t IMAGE_HEADER_SIZE = 12;
constexpr uint32_t SMALL_WIDTH = 8;
constexpr uint32_t SMALL_HEIGHT = 6;

constexpr lv_color_format_t OPAQUE = views_test::OPAQUE_IMAGE_FORMAT;
constexpr lv_color_format_t ALPHA = views_test::ALPHA_IMAGE_FORMAT;
// The other depth's opaque format: this build draws no file in it.
constexpr lv_color_format_t FOREIGN = LV_COLOR_DEPTH == 16 ? LV_COLOR_FORMAT_L8 : LV_COLOR_FORMAT_RGB565;

struct Bench {
    std::shared_ptr<eerie_leap::subsys::assets::AssetStore> store = views_test::MakeAssetStore("image-cache");
    std::shared_ptr<ImageCache> cache = std::make_shared<ImageCache>(
        views_test::MakeAssetsManager(AssetKind::IMAGES, store)
    );
};

void Save(const Bench& bench, const char* name, const std::vector<uint8_t>& file) {
    zassert_true(bench.store->Save(name, file));
}

bool IsDrawnByThisBuild(lv_color_format_t color_format) {
    return color_format == OPAQUE || color_format == ALPHA;
}

void* Setup() {
    views_test::EnsureTestDisplay();
    return nullptr;
}

} // namespace

ZTEST_SUITE(image_cache, NULL, Setup, NULL, views_test::CleanTestDisplay, NULL);

// The files LVGLImage.py makes, with padded rows among them, load as they are written: the header's numbers, the
// stride the tool chose and every pixel byte, in a block that starts where the draw units want it.
ZTEST(image_cache, test_the_files_the_tool_makes_load_as_written_and_aligned) {
    const ScopedLvglLock lock;
    const Bench bench;
    int loaded = 0;

    for(const auto& fixture : views_test::IMAGE_FILE_FIXTURES) {
        Save(bench, "fixture.lvimg", { fixture.bytes.begin(), fixture.bytes.end() });
        const auto image = bench.cache->Acquire("fixture.lvimg");

        if(fixture.is_compressed || !IsDrawnByThisBuild(fixture.color_format)) {
            zassert_is_null(image.get(), "%s was taken", fixture.name);
            continue;
        }

        zassert_not_null(image.get(), "%s was refused", fixture.name);
        const lv_image_dsc_t* descriptor = image->GetDescriptor();
        zassert_equal(descriptor->header.magic, LV_IMAGE_HEADER_MAGIC, "%s", fixture.name);
        zassert_equal(descriptor->header.cf, fixture.color_format, "%s", fixture.name);
        zassert_equal(descriptor->header.flags, 0, "%s", fixture.name);
        zassert_equal(descriptor->header.w, views_test::FIXTURE_WIDTH, "%s", fixture.name);
        zassert_equal(descriptor->header.h, views_test::FIXTURE_HEIGHT, "%s", fixture.name);
        zassert_equal(descriptor->header.stride, fixture.stride, "%s", fixture.name);
        zassert_equal(descriptor->data_size, fixture.bytes.size() - IMAGE_HEADER_SIZE, "%s", fixture.name);
        zassert_equal(image->GetByteCount(), fixture.bytes.size() - IMAGE_HEADER_SIZE, "%s", fixture.name);
        zassert_mem_equal(descriptor->data, fixture.bytes.data() + IMAGE_HEADER_SIZE, descriptor->data_size);
        zassert_equal(reinterpret_cast<uintptr_t>(descriptor->data) % LV_DRAW_BUF_ALIGN, 0U, "%s", fixture.name);
        ++loaded;
    }

    // Both of the build's formats, plain and padded, were among the files.
    zassert_true(loaded >= 4, "Only %d files were taken", loaded);
}

ZTEST(image_cache, test_a_file_is_loaded_once_and_shared_and_freed_with_its_last_holder) {
    const ScopedLvglLock lock;
    const Bench bench;
    Save(bench, "shared.lvimg", views_test::ImageFileBytes(ALPHA, 20, 10));
    const size_t pixel_bytes = views_test::ImagePixelBytes(ALPHA, 20, 10);

    zassert_equal(bench.cache->GetLoadedBytes(), 0U);
    auto first = bench.cache->Acquire("shared.lvimg");
    zassert_not_null(first.get());
    auto second = bench.cache->Acquire("shared.lvimg");
    zassert_equal(second.get(), first.get());
    zassert_equal(bench.cache->GetLoadedBytes(), pixel_bytes, "Counted once");

    first.reset();
    zassert_equal(bench.cache->GetLoadedBytes(), pixel_bytes, "One holder is left");
    second.reset();
    zassert_equal(bench.cache->GetLoadedBytes(), 0U);

    // A file that nobody holds is read again.
    zassert_not_null(bench.cache->Acquire("shared.lvimg").get());
}

ZTEST(image_cache, test_other_files_are_other_images) {
    const ScopedLvglLock lock;
    const Bench bench;
    Save(bench, "one.lvimg", views_test::ImageFileBytes(OPAQUE, 4, 4));
    Save(bench, "two.lvimg", views_test::ImageFileBytes(OPAQUE, 6, 4));

    const auto one = bench.cache->Acquire("one.lvimg");
    const auto two = bench.cache->Acquire("two.lvimg");

    zassert_not_null(one.get());
    zassert_not_null(two.get());
    zassert_not_equal(one.get(), two.get());
    zassert_equal(
        bench.cache->GetLoadedBytes(),
        views_test::ImagePixelBytes(OPAQUE, 4, 4) + views_test::ImagePixelBytes(OPAQUE, 6, 4)
    );
}

ZTEST(image_cache, test_a_padded_stride_is_kept_as_written) {
    const ScopedLvglLock lock;
    const Bench bench;
    constexpr uint32_t STRIDE = 64;
    Save(bench, "padded.lvimg", views_test::ImageFileBytes(OPAQUE, 5, 3, 0x5A, STRIDE));

    const auto image = bench.cache->Acquire("padded.lvimg");

    zassert_not_null(image.get());
    zassert_equal(image->GetDescriptor()->header.stride, STRIDE);
    zassert_equal(image->GetByteCount(), views_test::ImagePixelBytes(OPAQUE, 5, 3, STRIDE));
}

ZTEST(image_cache, test_what_cannot_load_gives_nothing) {
    const ScopedLvglLock lock;
    const Bench bench;
    const auto valid = views_test::ImageFileBytes(OPAQUE, 4, 4);

    auto shorter = valid;
    shorter.pop_back();
    auto longer = valid;
    longer.push_back(0);
    auto bad_magic = valid;
    bad_magic[0] = 0x00;

    Save(bench, "short.lvimg", shorter);
    Save(bench, "long.lvimg", longer);
    Save(bench, "magic.lvimg", bad_magic);
    Save(bench, "foreign.lvimg", views_test::ImageFileBytes(FOREIGN, 4, 4));
    Save(bench, "head_only.lvimg", { valid.begin(), valid.begin() + 5 });
    Save(bench, "text.lvimg", { 'n', 'o', 't', ' ', 'a', 'n', ' ', 'i', 'm', 'a', 'g', 'e', '!', '!' });

    for(const char* name : { "short.lvimg", "long.lvimg", "magic.lvimg", "foreign.lvimg", "head_only.lvimg",
            "text.lvimg", "absent.lvimg" }) {
        zassert_is_null(bench.cache->Acquire(name).get(), "%s was taken", name);
    }

    zassert_is_null(bench.cache->Acquire("").get());
    zassert_equal(bench.cache->GetLoadedBytes(), 0U);
}

ZTEST(image_cache, test_a_compressed_file_is_refused_until_the_build_decompresses) {
    const ScopedLvglLock lock;
    const Bench bench;

    for(const auto& fixture : views_test::IMAGE_FILE_FIXTURES) {
        if(!fixture.is_compressed)
            continue;

        Save(bench, "compressed.lvimg", { fixture.bytes.begin(), fixture.bytes.end() });
        zassert_is_null(bench.cache->Acquire("compressed.lvimg").get(), "%s was taken", fixture.name);
    }
}

ZTEST(image_cache, test_a_unit_without_images_gives_nothing) {
    const ScopedLvglLock lock;
    const Bench bench;
    Save(bench, "there.lvimg", views_test::ImageFileBytes(OPAQUE, 4, 4));

    ImageCache fonts_only(views_test::MakeAssetsManager(AssetKind::FONTS, bench.store));
    zassert_is_null(fonts_only.Acquire("there.lvimg").get());

    ImageCache none(std::make_shared<eerie_leap::subsys::assets::AssetsManager>());
    zassert_is_null(none.Acquire("there.lvimg").get());

    ImageCache no_manager(nullptr);
    zassert_is_null(no_manager.Acquire("there.lvimg").get());
}

// An upload or a delete changes the store's generation: the next acquire reads what the store holds now, and the
// image an icon already holds stays as it was.
ZTEST(image_cache, test_an_upload_changes_what_the_next_acquire_loads) {
    const ScopedLvglLock lock;
    const Bench bench;
    Save(bench, "changing.lvimg", views_test::ImageFileBytes(OPAQUE, 4, 4));

    const auto before = bench.cache->Acquire("changing.lvimg");
    zassert_not_null(before.get());
    zassert_equal(before->GetDescriptor()->header.w, 4U);

    Save(bench, "changing.lvimg", views_test::ImageFileBytes(OPAQUE, 9, 3));
    const auto after = bench.cache->Acquire("changing.lvimg");
    zassert_not_null(after.get());
    zassert_not_equal(after.get(), before.get());
    zassert_equal(after->GetDescriptor()->header.w, 9U);
    zassert_equal(before->GetDescriptor()->header.w, 4U);
    zassert_equal(
        bench.cache->GetLoadedBytes(),
        views_test::ImagePixelBytes(OPAQUE, 4, 4) + views_test::ImagePixelBytes(OPAQUE, 9, 3)
    );

    zassert_true(bench.store->Delete("changing.lvimg"));
    zassert_is_null(bench.cache->Acquire("changing.lvimg").get());
    zassert_equal(before->GetDescriptor()->header.w, 4U);
}

// A file whose pixels the heap cannot hold is refused, and the cache loads what fits afterwards.
ZTEST(image_cache, test_a_file_that_does_not_fit_the_memory_gives_nothing_and_the_next_one_loads) {
    // About 240 KB of pixels at either depth: more than the suite's 192 KB heap, less than its volume holds.
    constexpr uint32_t WIDE = LV_COLOR_DEPTH == 16 ? 400 : 600;
    constexpr uint32_t TALL = LV_COLOR_DEPTH == 16 ? 300 : 400;
    const Bench bench;
    if(!bench.store->Save("big.lvimg", views_test::ImageFileBytes(OPAQUE, WIDE, TALL))) {
        bench.store->Delete("big.lvimg");
        // Before the lock: a skip leaves the test's scope without running destructors.
        ztest_test_skip();
    }

    const ScopedLvglLock lock;
    zassert_is_null(bench.cache->Acquire("big.lvimg").get());
    zassert_true(bench.store->Delete("big.lvimg"));

    Save(bench, "small.lvimg", views_test::ImageFileBytes(OPAQUE, SMALL_WIDTH, SMALL_HEIGHT));
    zassert_not_null(bench.cache->Acquire("small.lvimg").get());
}
