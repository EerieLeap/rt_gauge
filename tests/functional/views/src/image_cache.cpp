#include <algorithm>
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
// The two headers a compressed file starts with: the image header, then the compressed-data header.
constexpr size_t COMPRESSED_HEAD_SIZE = 24;
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

#ifdef CONFIG_EERIE_LEAP_UI_IMAGE_LZ4
constexpr bool BUILD_DECOMPRESSES = true;
#else
constexpr bool BUILD_DECOMPRESSES = false;
#endif

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

    int compressed_loaded = 0;

    for(const auto& fixture : views_test::IMAGE_FILE_FIXTURES) {
        Save(bench, "fixture.lvimg", { fixture.bytes.begin(), fixture.bytes.end() });
        const auto image = bench.cache->Acquire("fixture.lvimg");

        if((fixture.is_compressed && !BUILD_DECOMPRESSES) || !IsDrawnByThisBuild(fixture.color_format)) {
            zassert_is_null(image.get(), "%s was taken", fixture.name);
            continue;
        }

        // A compressed file holds the pixels of its twin, the same picture written without compression.
        const auto twin = fixture.is_compressed ? views_test::UncompressedTwinOf(fixture.name) : fixture.bytes;
        zassert_true(twin.size() > IMAGE_HEADER_SIZE, "%s has no twin", fixture.name);

        zassert_not_null(image.get(), "%s was refused", fixture.name);
        const lv_image_dsc_t* descriptor = image->GetDescriptor();
        zassert_equal(descriptor->header.magic, LV_IMAGE_HEADER_MAGIC, "%s", fixture.name);
        zassert_equal(descriptor->header.cf, fixture.color_format, "%s", fixture.name);
        zassert_equal(descriptor->header.flags, 0, "%s", fixture.name);
        zassert_equal(descriptor->header.w, views_test::FIXTURE_WIDTH, "%s", fixture.name);
        zassert_equal(descriptor->header.h, views_test::FIXTURE_HEIGHT, "%s", fixture.name);
        zassert_equal(descriptor->header.stride, fixture.stride, "%s", fixture.name);
        zassert_equal(descriptor->data_size, twin.size() - IMAGE_HEADER_SIZE, "%s", fixture.name);
        zassert_equal(image->GetByteCount(), twin.size() - IMAGE_HEADER_SIZE, "%s", fixture.name);
        zassert_mem_equal(descriptor->data, twin.data() + IMAGE_HEADER_SIZE, descriptor->data_size);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast): the address is compared with an alignment.
        zassert_equal(reinterpret_cast<uintptr_t>(descriptor->data) % LV_DRAW_BUF_ALIGN, 0U, "%s", fixture.name);
        ++loaded;
        if(fixture.is_compressed)
            ++compressed_loaded;
    }

    // Both of the build's formats, plain and padded, were among the files, and so were its two compressed ones.
    zassert_true(loaded >= 4, "Only %d files were taken", loaded);
    zassert_equal(compressed_loaded, BUILD_DECOMPRESSES ? 2 : 0);
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

#ifndef CONFIG_EERIE_LEAP_UI_IMAGE_LZ4
ZTEST(image_cache, test_a_compressed_file_is_refused_by_a_build_that_decompresses_nothing) {
    const ScopedLvglLock lock;
    const Bench bench;

    for(const auto& fixture : views_test::IMAGE_FILE_FIXTURES) {
        if(!fixture.is_compressed)
            continue;

        Save(bench, "compressed.lvimg", { fixture.bytes.begin(), fixture.bytes.end() });
        zassert_is_null(bench.cache->Acquire("compressed.lvimg").get(), "%s was taken", fixture.name);
    }
}
#else
namespace {

// Blocks python-lz4 wrote (lz4.block.compress with store_size=False) that expand to a repeating pattern and to zeros
// by copying their own output: the matches an image of flat colour is made of.
constexpr std::array<uint8_t, 18> REPEATING_200_BLOCK = { 0x8F, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x08,
    0x00, 0xA8, 0x50, 0x64, 0x65, 0x66, 0x67, 0x68 };
constexpr std::array<uint8_t, 14> ZEROS_1000_BLOCK = { 0x1F, 0x00, 0x01, 0x00, 0xFF, 0xFF, 0xFF, 0xD2, 0x50, 0x00, 0x00,
    0x00, 0x00, 0x00 };
constexpr uint32_t REPEATING_200_BYTES = 200;
constexpr uint32_t ZEROS_1000_BYTES = 1000;

// Images of exactly those many pixel bytes in the build's opaque format.
constexpr uint32_t REPEATING_WIDTH = LV_COLOR_DEPTH == 16 ? 10 : 20;
constexpr uint32_t REPEATING_HEIGHT = 10;
constexpr uint32_t ZEROS_WIDTH = 25;
constexpr uint32_t ZEROS_HEIGHT = LV_COLOR_DEPTH == 16 ? 20 : 40;

std::vector<uint8_t> RepeatingPixels() {
    std::vector<uint8_t> pixels;
    pixels.reserve(REPEATING_200_BYTES);
    for(uint32_t i = 0; i < REPEATING_200_BYTES; ++i)
        pixels.push_back(static_cast<uint8_t>('a' + (i % 8)));
    return pixels;
}

} // namespace

// Runs and overlapping copies, which the small fixtures hold none of.
ZTEST(image_cache, test_blocks_that_copy_their_own_output_expand_to_the_pixels) {
    const ScopedLvglLock lock;
    const Bench bench;
    Save(
        bench, "repeating.lvimg",
        views_test::CompressedImageFileAround(
            OPAQUE, REPEATING_WIDTH, REPEATING_HEIGHT, 0, REPEATING_200_BLOCK, REPEATING_200_BYTES
        )
    );
    Save(
        bench, "zeros.lvimg",
        views_test::CompressedImageFileAround(OPAQUE, ZEROS_WIDTH, ZEROS_HEIGHT, 0, ZEROS_1000_BLOCK, ZEROS_1000_BYTES)
    );

    const auto repeating = bench.cache->Acquire("repeating.lvimg");
    zassert_not_null(repeating.get());
    const auto expected = RepeatingPixels();
    zassert_equal(repeating->GetByteCount(), expected.size());
    zassert_mem_equal(repeating->GetDescriptor()->data, expected.data(), expected.size());

    const auto zeros = bench.cache->Acquire("zeros.lvimg");
    zassert_not_null(zeros.get());
    zassert_equal(zeros->GetByteCount(), ZEROS_1000_BYTES);
    const std::vector<uint8_t> flat(ZEROS_1000_BYTES, 0);
    zassert_mem_equal(zeros->GetDescriptor()->data, flat.data(), flat.size());

    // The cache counts what is in RAM, not what the files took.
    zassert_equal(bench.cache->GetLoadedBytes(), REPEATING_200_BYTES + ZEROS_1000_BYTES);
}

// A literal-only block is valid LZ4 too: a picture that does not compress, stored in the compressed form.
ZTEST(image_cache, test_a_block_of_literals_alone_loads_as_the_pixels_it_holds) {
    const ScopedLvglLock lock;
    const Bench bench;
    std::vector<uint8_t> pixels(views_test::ImagePixelBytes(ALPHA, SMALL_WIDTH, SMALL_HEIGHT));
    for(size_t i = 0; i < pixels.size(); ++i)
        pixels[i] = static_cast<uint8_t>((i * 7) + 3);
    const auto block = views_test::Lz4LiteralBlock(pixels);
    Save(
        bench, "literals.lvimg",
        views_test::CompressedImageFileAround(
            ALPHA, SMALL_WIDTH, SMALL_HEIGHT, 0, block, static_cast<uint32_t>(pixels.size())
        )
    );

    const auto image = bench.cache->Acquire("literals.lvimg");

    zassert_not_null(image.get());
    zassert_equal(image->GetByteCount(), pixels.size());
    zassert_mem_equal(image->GetDescriptor()->data, pixels.data(), pixels.size());
}

// A file whose block does not expand to exactly the pixels its header promises draws nothing, and the cache loads
// what is sound afterwards: nothing of the failed attempts stays in RAM.
ZTEST(image_cache, test_a_compressed_file_that_does_not_expand_to_its_pixels_gives_nothing) {
    const ScopedLvglLock lock;
    const Bench bench;
    const auto pixel_bytes = static_cast<uint32_t>(views_test::ImagePixelBytes(OPAQUE, SMALL_WIDTH, SMALL_HEIGHT));

    // Fewer bytes than promised.
    const std::vector<uint8_t> fewer(pixel_bytes - 1, 0x5A);
    Save(
        bench, "fewer.lvimg",
        views_test::CompressedImageFileAround(
            OPAQUE, SMALL_WIDTH, SMALL_HEIGHT, 0, views_test::Lz4LiteralBlock(fewer), pixel_bytes
        )
    );
    // More bytes than the pixels can hold: the decoder stops at the end of the buffer.
    const std::vector<uint8_t> more(pixel_bytes + 1, 0x5A);
    Save(
        bench, "more.lvimg",
        views_test::CompressedImageFileAround(
            OPAQUE, SMALL_WIDTH, SMALL_HEIGHT, 0, views_test::Lz4LiteralBlock(more), pixel_bytes
        )
    );
    // A block that runs out of input in the middle of a length.
    const std::vector<uint8_t> unfinished(5, 0xFF);
    Save(
        bench, "unfinished.lvimg",
        views_test::CompressedImageFileAround(OPAQUE, SMALL_WIDTH, SMALL_HEIGHT, 0, unfinished, pixel_bytes)
    );
    // A copy from before the start of the output.
    const std::array<uint8_t, 6> before_start = { 0x04, 0x00, 0x00, 0x00, 0x00, 0x00 };
    Save(
        bench, "before_start.lvimg",
        views_test::CompressedImageFileAround(OPAQUE, SMALL_WIDTH, SMALL_HEIGHT, 0, before_start, pixel_bytes)
    );

    for(const char* name : { "fewer.lvimg", "more.lvimg", "unfinished.lvimg", "before_start.lvimg" })
        zassert_is_null(bench.cache->Acquire(name).get(), "%s was taken", name);

    zassert_equal(bench.cache->GetLoadedBytes(), 0U);

    const std::vector<uint8_t> pixels(pixel_bytes, 0x5A);
    Save(
        bench, "sound.lvimg",
        views_test::CompressedImageFileAround(
            OPAQUE, SMALL_WIDTH, SMALL_HEIGHT, 0, views_test::Lz4LiteralBlock(pixels), pixel_bytes
        )
    );
    const auto sound = bench.cache->Acquire("sound.lvimg");
    zassert_not_null(sound.get());
    zassert_equal(bench.cache->GetLoadedBytes(), pixel_bytes);
}

// The tool's own files with their data damaged: the head is sound, so the parser takes them, and the expansion is
// what finds the damage.
ZTEST(image_cache, test_the_tools_compressed_files_with_damaged_data_give_nothing) {
    const ScopedLvglLock lock;
    const Bench bench;
    int damaged = 0;

    for(const auto& fixture : views_test::IMAGE_FILE_FIXTURES) {
        if(!fixture.is_compressed || !IsDrawnByThisBuild(fixture.color_format))
            continue;

        // Every data byte at 0xFF: a literal length that does not end before the input does.
        std::vector<uint8_t> file(fixture.bytes.begin(), fixture.bytes.end());
        std::fill(file.begin() + COMPRESSED_HEAD_SIZE, file.end(), 0xFF);
        Save(bench, "damaged.lvimg", file);
        zassert_is_null(bench.cache->Acquire("damaged.lvimg").get(), "%s was taken", fixture.name);

        // The first token says no literals, so the copy that follows reaches before the start of the output.
        file.assign(fixture.bytes.begin(), fixture.bytes.end());
        file[COMPRESSED_HEAD_SIZE] = 0x00;
        Save(bench, "damaged.lvimg", file);
        zassert_is_null(
            bench.cache->Acquire("damaged.lvimg").get(), "%s with its first token cleared was taken", fixture.name
        );
        ++damaged;
    }

    zassert_equal(damaged, 2);
    zassert_equal(bench.cache->GetLoadedBytes(), 0U);
}
#endif

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
