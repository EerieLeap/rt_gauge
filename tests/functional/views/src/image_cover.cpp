// LVGL patch lvgl_image_opaque_alpha.patch: an lv_image with a variable RGB565A8 source whose alpha bytes
// are all 0xFF covers what lies beneath it and draws through its RGB565 view; every other image does not cover.
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <zephyr/ztest.h>

#include <lvgl.h>
extern "C" {
#include <core/lv_obj_event_private.h> // lv_cover_check_info_t
}

#include "domain/ui_domain/lvgl_lock.h"
#include "subsys/assets/asset_kind.h"
#include "views/utilities/image_cache.h"
#include "views_test_support.h"

namespace {

constexpr int32_t IMAGE_SIZE = 8;
constexpr int32_t IMAGE_X = 4;
constexpr int32_t IMAGE_Y = 4;
constexpr uint16_t RED_565 = 0xF800;
constexpr uint16_t GREEN_565 = 0x07E0;

constexpr bool HAS_RGB565A8 = LV_COLOR_FORMAT_NATIVE_WITH_ALPHA == LV_COLOR_FORMAT_RGB565A8;

// An RGB565A8 image: the left half red, the right half green; the alpha of the pixels is chosen per column.
class TestImage {
public:
    explicit TestImage(uint8_t left_alpha = 0xFF, uint8_t right_alpha = 0xFF) {
        const uint32_t stride = IMAGE_SIZE * 2;
        data_.resize((stride + stride / 2) * IMAGE_SIZE);
        for(int32_t y = 0; y < IMAGE_SIZE; ++y) {
            for(int32_t x = 0; x < IMAGE_SIZE; ++x) {
                const uint16_t color = x < IMAGE_SIZE / 2 ? RED_565 : GREEN_565;
                data_[stride * y + x * 2] = color & 0xFF;
                data_[stride * y + x * 2 + 1] = color >> 8;
                data_[stride * IMAGE_SIZE + (stride / 2) * y + x] = x < IMAGE_SIZE / 2 ? left_alpha : right_alpha;
            }
        }
        dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc_.header.cf = LV_COLOR_FORMAT_RGB565A8;
        dsc_.header.w = IMAGE_SIZE;
        dsc_.header.h = IMAGE_SIZE;
        dsc_.header.stride = stride;
        dsc_.data_size = data_.size();
        dsc_.data = data_.data();
    }

    // One alpha byte below 0xFF in an otherwise opaque image.
    void LowerOneAlpha() {
        data_[(IMAGE_SIZE * 2) * IMAGE_SIZE + 3] = 0xFE;
    }

    const lv_image_dsc_t* Dsc() const {
        return &dsc_;
    }

private:
    std::vector<uint8_t> data_;
    lv_image_dsc_t dsc_{};
};

struct Scene {
    lv_obj_t* parent;
    lv_obj_t* image;
};

// A 40x40 parent with an opaque blue background and the image at (IMAGE_X, IMAGE_Y), transparent background.
Scene MakeScene(const lv_image_dsc_t* source) {
    lv_obj_t* parent = lv_obj_create(lv_screen_active());
    lv_obj_remove_style_all(parent);
    lv_obj_set_size(parent, 40, 40);
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x0000FF), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_t* image = lv_image_create(parent);
    lv_image_set_src(image, source);
    lv_obj_set_pos(image, IMAGE_X, IMAGE_Y);
    lv_obj_update_layout(parent);
    zassert_equal(lv_obj_get_width(image), IMAGE_SIZE);
    zassert_equal(lv_obj_get_height(image), IMAGE_SIZE);
    return { parent, image };
}

lv_cover_res_t CoverCheck(lv_obj_t* image, const lv_area_t& area) {
    lv_cover_check_info_t info{ .res = LV_COVER_RES_COVER, .area = &area };
    lv_obj_send_event(image, LV_EVENT_COVER_CHECK, &info);
    return info.res;
}

// An area strictly inside the image.
lv_cover_res_t CoverCheckInside(lv_obj_t* image) {
    lv_area_t area;
    lv_obj_get_coords(image, &area);
    lv_area_set(&area, area.x1 + 1, area.y1 + 1, area.x2 - 1, area.y2 - 1);
    return CoverCheck(image, area);
}

struct Pixel {
    uint8_t red, green, blue;
};

Pixel PixelAt(lv_obj_t* parent, int32_t x, int32_t y) {
    lv_obj_update_layout(parent);
    std::unique_ptr<lv_draw_buf_t, decltype(&lv_draw_buf_destroy)> snapshot(
        lv_snapshot_take(parent, LV_COLOR_FORMAT_ARGB8888), lv_draw_buf_destroy
    );
    zassert_not_null(snapshot);
    const auto& pixel = views_test::PixelRow(*snapshot, y)[x];
    return { pixel.red, pixel.green, pixel.blue };
}

void CheckPixel(lv_obj_t* parent, int32_t x, int32_t y, Pixel expected) {
    const auto actual = PixelAt(parent, x, y);
    zassert_true(
        actual.red == expected.red && actual.green == expected.green && actual.blue == expected.blue,
        "pixel %d,%d = %u,%u,%u, expected %u,%u,%u", x, y, actual.red, actual.green, actual.blue, expected.red,
        expected.green, expected.blue
    );
}

void* Setup() {
    views_test::EnsureTestDisplay();
    return nullptr;
}

} // namespace

ZTEST_SUITE(image_cover, NULL, Setup, NULL, views_test::CleanTestDisplay, NULL);

ZTEST(image_cover, test_opaque_image_covers_area_inside) {
    if(!HAS_RGB565A8)
        ztest_test_skip();
    eerie_leap::domain::ui_domain::ScopedLvglLock lock;
    TestImage opaque;
    const auto scene = MakeScene(opaque.Dsc());
    zassert_equal(CoverCheckInside(scene.image), LV_COVER_RES_COVER);
}

ZTEST(image_cover, test_image_with_a_transparent_byte_does_not_cover_and_resets_on_new_source) {
    if(!HAS_RGB565A8)
        ztest_test_skip();
    eerie_leap::domain::ui_domain::ScopedLvglLock lock;
    TestImage opaque;
    TestImage holed;
    holed.LowerOneAlpha();
    const auto scene = MakeScene(opaque.Dsc());
    zassert_equal(CoverCheckInside(scene.image), LV_COVER_RES_COVER);
    // The cached alpha state of the first source must not outlive it.
    lv_image_set_src(scene.image, holed.Dsc());
    zassert_equal(CoverCheckInside(scene.image), LV_COVER_RES_NOT_COVER);
    lv_image_set_src(scene.image, opaque.Dsc());
    zassert_equal(CoverCheckInside(scene.image), LV_COVER_RES_COVER);
}

ZTEST(image_cover, test_colorkey_image_does_not_cover) {
    if(!HAS_RGB565A8)
        ztest_test_skip();
    eerie_leap::domain::ui_domain::ScopedLvglLock lock;
    static const lv_image_colorkey_t KEY{ .low = lv_color_hex(0x000000), .high = lv_color_hex(0x000000) };
    TestImage opaque;
    const auto scene = MakeScene(opaque.Dsc());
    lv_obj_set_style_image_colorkey(scene.image, &KEY, 0);
    zassert_not_equal(CoverCheckInside(scene.image), LV_COVER_RES_COVER);
}

ZTEST(image_cover, test_translucent_image_does_not_cover) {
    if(!HAS_RGB565A8)
        ztest_test_skip();
    eerie_leap::domain::ui_domain::ScopedLvglLock lock;
    TestImage opaque;
    const auto scene = MakeScene(opaque.Dsc());
    lv_obj_set_style_image_opa(scene.image, 200, 0);
    zassert_not_equal(CoverCheckInside(scene.image), LV_COVER_RES_COVER);
}

ZTEST(image_cover, test_rotated_image_does_not_cover) {
    if(!HAS_RGB565A8)
        ztest_test_skip();
    eerie_leap::domain::ui_domain::ScopedLvglLock lock;
    TestImage opaque;
    const auto scene = MakeScene(opaque.Dsc());
    lv_image_set_rotation(scene.image, 300);
    zassert_not_equal(CoverCheckInside(scene.image), LV_COVER_RES_COVER);
}

ZTEST(image_cover, test_scaled_image_does_not_cover) {
    if(!HAS_RGB565A8)
        ztest_test_skip();
    eerie_leap::domain::ui_domain::ScopedLvglLock lock;
    TestImage opaque;
    const auto scene = MakeScene(opaque.Dsc());
    lv_image_set_scale(scene.image, 512);
    zassert_not_equal(CoverCheckInside(scene.image), LV_COVER_RES_COVER);
}

ZTEST(image_cover, test_area_larger_than_image_is_not_covered) {
    if(!HAS_RGB565A8)
        ztest_test_skip();
    eerie_leap::domain::ui_domain::ScopedLvglLock lock;
    TestImage opaque;
    const auto scene = MakeScene(opaque.Dsc());
    lv_area_t area;
    lv_obj_get_coords(scene.image, &area);
    lv_area_set(&area, area.x1 - 2, area.y1 - 2, area.x2 + 2, area.y2 + 2);
    zassert_not_equal(CoverCheck(scene.image, area), LV_COVER_RES_COVER);
}

ZTEST(image_cover, test_opaque_image_draws_its_own_colors) {
    if(!HAS_RGB565A8)
        ztest_test_skip();
    eerie_leap::domain::ui_domain::ScopedLvglLock lock;
    TestImage opaque;
    const auto scene = MakeScene(opaque.Dsc());
    CheckPixel(scene.parent, IMAGE_X + 1, IMAGE_Y + 4, { 255, 0, 0 });
    CheckPixel(scene.parent, IMAGE_X + 6, IMAGE_Y + 4, { 0, 255, 0 });
    CheckPixel(scene.parent, 1, 1, { 0, 0, 255 }); // Outside the image: the parent.
}

ZTEST(image_cover, test_transparent_pixels_show_the_parent) {
    if(!HAS_RGB565A8)
        ztest_test_skip();
    eerie_leap::domain::ui_domain::ScopedLvglLock lock;
    TestImage holed(0x00, 0xFF);
    const auto scene = MakeScene(holed.Dsc());
    CheckPixel(scene.parent, IMAGE_X + 1, IMAGE_Y + 4, { 0, 0, 255 }); // Alpha 0: the blue parent.
    CheckPixel(scene.parent, IMAGE_X + 6, IMAGE_Y + 4, { 0, 255, 0 });
}

// ======== Image files (.lvimg) ========

namespace {

constexpr bool HAS_RGB565 = LV_COLOR_DEPTH == 16;

// An image file of an RGB565 picture, the left half red and the right half green, or of the same picture as
// RGB565A8 with the alpha of each column chosen.
std::vector<uint8_t>
PictureFile(lv_color_format_t color_format, uint8_t left_alpha = 0xFF, uint8_t right_alpha = 0xFF) {
    const uint32_t stride = IMAGE_SIZE * 2;
    std::vector<uint8_t> pixels(views_test::ImagePixelBytes(color_format, IMAGE_SIZE, IMAGE_SIZE), 0);
    for(int32_t y = 0; y < IMAGE_SIZE; ++y) {
        for(int32_t x = 0; x < IMAGE_SIZE; ++x) {
            const uint16_t color = x < IMAGE_SIZE / 2 ? RED_565 : GREEN_565;
            pixels[stride * y + x * 2] = color & 0xFF;
            pixels[stride * y + x * 2 + 1] = color >> 8;
            if(color_format == LV_COLOR_FORMAT_RGB565A8)
                pixels[stride * IMAGE_SIZE + (stride / 2) * y + x] = x < IMAGE_SIZE / 2 ? left_alpha : right_alpha;
        }
    }
    return views_test::ImageFileAround(color_format, IMAGE_SIZE, IMAGE_SIZE, stride, pixels);
}

// An image file as the cache loads it.
struct LoadedFile {
    std::shared_ptr<eerie_leap::views::utilities::ImageCache> cache;
    std::shared_ptr<const eerie_leap::views::utilities::LoadedImage> image;
};

LoadedFile Load(const std::vector<uint8_t>& file) {
    auto store = views_test::MakeAssetStore("image-cover");
    zassert_true(store->Save("picture.lvimg", file));
    auto cache = std::make_shared<eerie_leap::views::utilities::ImageCache>(
        views_test::MakeAssetsManager(eerie_leap::subsys::assets::AssetKind::IMAGES, store)
    );
    auto image = cache->Acquire("picture.lvimg");
    zassert_not_null(image.get());
    return { std::move(cache), std::move(image) };
}

} // namespace

// An opaque RGB565 file is an RGB565 source: nothing to scan for alpha, and it covers what lies beneath it.
ZTEST(image_cover, test_an_rgb565_image_file_covers_and_draws_its_own_colors) {
    if(!HAS_RGB565)
        ztest_test_skip();
    eerie_leap::domain::ui_domain::ScopedLvglLock lock;
    const auto loaded = Load(PictureFile(LV_COLOR_FORMAT_RGB565));
    zassert_equal(loaded.image->GetDescriptor()->header.cf, LV_COLOR_FORMAT_RGB565);

    const auto scene = MakeScene(loaded.image->GetDescriptor());

    zassert_equal(CoverCheckInside(scene.image), LV_COVER_RES_COVER);
    CheckPixel(scene.parent, IMAGE_X + 1, IMAGE_Y + 4, { 255, 0, 0 });
    CheckPixel(scene.parent, IMAGE_X + 6, IMAGE_Y + 4, { 0, 255, 0 });
    CheckPixel(scene.parent, 1, 1, { 0, 0, 255 });
}

// An opaque RGB565A8 file still covers, by the scan of its alpha plane, as the same picture in memory does.
ZTEST(image_cover, test_an_opaque_rgb565a8_image_file_covers_and_a_holed_one_does_not) {
    if(!HAS_RGB565)
        ztest_test_skip();
    eerie_leap::domain::ui_domain::ScopedLvglLock lock;

    const auto opaque = Load(PictureFile(LV_COLOR_FORMAT_RGB565A8));
    const auto opaque_scene = MakeScene(opaque.image->GetDescriptor());
    zassert_equal(CoverCheckInside(opaque_scene.image), LV_COVER_RES_COVER);

    const auto holed = Load(PictureFile(LV_COLOR_FORMAT_RGB565A8, 0x00, 0xFF));
    const auto holed_scene = MakeScene(holed.image->GetDescriptor());
    zassert_not_equal(CoverCheckInside(holed_scene.image), LV_COVER_RES_COVER);
    CheckPixel(holed_scene.parent, IMAGE_X + 1, IMAGE_Y + 4, { 0, 0, 255 });
    CheckPixel(holed_scene.parent, IMAGE_X + 6, IMAGE_Y + 4, { 0, 255, 0 });
}
