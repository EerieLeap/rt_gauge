// Golden output of LVGL's software image transform (rotation, scale, pivot, antialiasing) for every source
// color format the build enables.
//
// The needle of a dial is a rotated image, and the plan for the display performance (Phase 3) changes the chunk
// loop of lv_draw_sw_img.c so that rotated images skip pixels outside the image. That must not change one bit of
// the output, so the output of the unpatched LVGL is recorded here: each group of draws (format, antialias, scale,
// pivot) is drawn onto a canvas of every target format (ARGB8888, and RGB565, which has no alpha: the patch narrows the
// chunks only on a target without alpha), and the CRC32 over the digests (FNV-1a of every 32-bit word) of the canvas
// after every draw, is compared with GOLDEN and GOLDEN_RGB565 below.
//
// To record the table again (after a deliberate change of the output): set REGENERATE to true, build and run the
// suite on native_sim (`zephyr.exe -test="image_transform_golden::*"`), paste the printed rows between the
// markers into the table the marker names, and set REGENERATE back to false. The table must come from an LVGL
// without the patch: revert it first (move the patch out of app/patches/modules/lvgl, git apply --reverse).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <zephyr/sys/crc.h>
#include <zephyr/sys/printk.h>
#include <zephyr/ztest.h>

#include <lvgl.h>

#include "views_test_support.h"

namespace {

constexpr bool REGENERATE = false;

// The canvas of the generic groups; the pivot sits at the canvas center. A 16x24 source at scale 300 is about
// 19x28, a single chunk of the draw for most formats; at scale 768 it is 48x72 and turns in a box of 87x87, which the
// canvas clips for most angles and which takes several chunks.
constexpr int32_t CANVAS_SIZE = 64;
// The canvas of the needle groups: a 15x220 image whose pivot (8, 212) sits at the canvas center.
constexpr int32_t NEEDLE_CANVAS_SIZE = 112;

constexpr int32_t IMAGE_W = 16;
constexpr int32_t IMAGE_H = 24;
constexpr int32_t NEEDLE_W = 15;
constexpr int32_t NEEDLE_H = 220;

constexpr uint32_t BACKGROUND = 0xFF305070;

struct Rgba {
    uint8_t r, g, b, a;
};

// Gradients in red and green, a hard-edged pattern in blue, an alpha gradient along x, a fully transparent
// stripe, fully opaque top and left edges and a half-transparent bottom-right block.
Rgba Sample(int32_t x, int32_t y, int32_t w, int32_t h) {
    Rgba p{};
    p.r = static_cast<uint8_t>(x * 255 / (w - 1));
    p.g = static_cast<uint8_t>(y * 255 / (h - 1));
    p.b = ((x ^ y) & 4) != 0 ? 230 : 25;
    int32_t a = 40 + x * 215 / (w - 1);
    if(x >= w / 3 && x < w / 3 + 2) {
        a = 0;
    }
    if(x == 0 || y == 0) {
        a = 255;
    }
    if(x >= w * 2 / 3 && y >= h * 2 / 3) {
        a = 128;
    }
    p.a = static_cast<uint8_t>(a);
    return p;
}

uint16_t To565(const Rgba& p) {
    return static_cast<uint16_t>(((p.r >> 3) << 11) | ((p.g >> 2) << 5) | (p.b >> 3));
}

uint8_t Premultiply(uint8_t c, uint8_t a) {
    return static_cast<uint8_t>((c * a + 127) / 255);
}

uint8_t Gray(const Rgba& p) {
    return static_cast<uint8_t>((p.r * 77 + p.g * 150 + p.b * 29) >> 8);
}

struct Format {
    const char* name;
    lv_color_format_t cf;
    bool has_alpha;
    bool recolor; // A8 has no color of its own
};

// clang-format off: a table.
constexpr Format FORMATS[] = {
#if LV_DRAW_SW_SUPPORT_RGB565A8
    { "RGB565A8", LV_COLOR_FORMAT_RGB565A8, true, false },
#endif
#if LV_DRAW_SW_SUPPORT_RGB565
    { "RGB565", LV_COLOR_FORMAT_RGB565, false, false },
#endif
#if LV_DRAW_SW_SUPPORT_RGB565_SWAPPED
    { "RGB565_SWAPPED", LV_COLOR_FORMAT_RGB565_SWAPPED, false, false },
#endif
#if LV_DRAW_SW_SUPPORT_ARGB8888
    { "ARGB8888", LV_COLOR_FORMAT_ARGB8888, true, false },
#endif
#if LV_DRAW_SW_SUPPORT_ARGB8888_PREMULTIPLIED
    { "ARGB8888_PREMULTIPLIED", LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED, true, false },
#endif
#if LV_DRAW_SW_SUPPORT_RGB888
    { "RGB888", LV_COLOR_FORMAT_RGB888, false, false },
#endif
#if LV_DRAW_SW_SUPPORT_XRGB8888
    { "XRGB8888", LV_COLOR_FORMAT_XRGB8888, false, false },
#endif
#if LV_DRAW_SW_SUPPORT_A8
    { "A8", LV_COLOR_FORMAT_A8, true, true },
#endif
#if LV_DRAW_SW_SUPPORT_L8
    { "L8", LV_COLOR_FORMAT_L8, false, false },
#endif
#if LV_DRAW_SW_SUPPORT_AL88
    { "AL88", LV_COLOR_FORMAT_AL88, true, false },
#endif
};
// clang-format on

constexpr size_t FORMAT_COUNT = sizeof(FORMATS) / sizeof(FORMATS[0]);

// A source image of the format with its pixel data.
class Source {
public:
    Source(lv_color_format_t cf, int32_t w, int32_t h) {
        const uint32_t px = lv_color_format_get_size(cf);
        const uint32_t stride = static_cast<uint32_t>(w) * px;
        const bool plane = cf == LV_COLOR_FORMAT_RGB565A8;
        data_.assign(stride * h + (plane ? static_cast<size_t>(w) * h : 0), 0);
        uint8_t* mask = data_.data() + static_cast<size_t>(stride) * h;
        for(int32_t y = 0; y < h; ++y) {
            for(int32_t x = 0; x < w; ++x) {
                const Rgba p = Sample(x, y, w, h);
                uint8_t* d = data_.data() + static_cast<size_t>(stride) * y + static_cast<size_t>(x) * px;
                switch(cf) {
                    case LV_COLOR_FORMAT_RGB565A8:
                    case LV_COLOR_FORMAT_RGB565: {
                        const uint16_t v = To565(p);
                        d[0] = v & 0xFF;
                        d[1] = v >> 8;
                        if(plane) {
                            mask[static_cast<size_t>(w) * y + x] = p.a;
                        }
                        break;
                    }
                    case LV_COLOR_FORMAT_RGB565_SWAPPED: {
                        const uint16_t v = To565(p);
                        d[0] = v >> 8;
                        d[1] = v & 0xFF;
                        break;
                    }
                    case LV_COLOR_FORMAT_ARGB8888:
                    case LV_COLOR_FORMAT_XRGB8888:
                        d[0] = p.b;
                        d[1] = p.g;
                        d[2] = p.r;
                        d[3] = cf == LV_COLOR_FORMAT_ARGB8888 ? p.a : 0xFF;
                        break;
                    case LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED:
                        d[0] = Premultiply(p.b, p.a);
                        d[1] = Premultiply(p.g, p.a);
                        d[2] = Premultiply(p.r, p.a);
                        d[3] = p.a;
                        break;
                    case LV_COLOR_FORMAT_RGB888:
                        d[0] = p.b;
                        d[1] = p.g;
                        d[2] = p.r;
                        break;
                    case LV_COLOR_FORMAT_A8:
                        d[0] = p.a;
                        break;
                    case LV_COLOR_FORMAT_L8:
                        d[0] = Gray(p);
                        break;
                    case LV_COLOR_FORMAT_AL88:
                        d[0] = Gray(p);
                        d[1] = p.a;
                        break;
                    default:
                        break;
                }
            }
        }
        dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        dsc_.header.cf = cf;
        dsc_.header.w = w;
        dsc_.header.h = h;
        dsc_.header.stride = stride;
        dsc_.data_size = data_.size();
        dsc_.data = data_.data();
    }

    const lv_image_dsc_t* Dsc() const {
        return &dsc_;
    }

private:
    std::vector<uint8_t> data_;
    lv_image_dsc_t dsc_{};
};

// Whole degrees in steps (7 degrees; 14 for the anisotropic scale, 30 for the needle), then angles with a tenth-of-a-degree part (LVGL rotates in 0.1 degree).
std::vector<int32_t> Angles(int32_t step_degrees) {
    std::vector<int32_t> angles;
    for(int32_t degrees = 0; degrees < 360; degrees += step_degrees) {
        angles.push_back(degrees * 10);
    }
    for(const int32_t tenths : { 5, 451, 1799, 2255 }) {
        angles.push_back(tenths);
    }
    return angles;
}

enum class Kind : uint8_t {
    Rotate,
    ScaleOnly,
    Needle
};

struct Pivot {
    const char* name;
    int32_t x, y;
};

struct Group {
    char name[48];
    Kind kind;
    bool antialias;
    int32_t scale_x, scale_y;
    Pivot pivot;
    lv_opa_t recolor_opa; // 0: none (A8 sources are always recolored at full opacity)
};

constexpr Pivot CENTER = { "center", IMAGE_W / 2, IMAGE_H / 2 };
constexpr Pivot CORNER = { "corner", 0, 0 };
constexpr Pivot OFF_CENTER = { "off-center", 11, 19 };
constexpr Pivot NEEDLE_PIVOT = { "needle", 8, 212 };

// The groups of one format, in the order of the columns of GOLDEN.
constexpr size_t GROUPS_PER_FORMAT = 20;

std::vector<Group> Groups() {
    std::vector<Group> groups;
    auto add = [&groups](
                   Kind kind, bool aa, int32_t sx, int32_t sy, const Pivot& pivot, const char* what,
                   lv_opa_t recolor_opa = 0
               ) {
        Group g{};
        std::snprintf(g.name, sizeof(g.name), "%s aa=%d scale=%dx%d pivot=%s", what, aa, sx, sy, pivot.name);
        g.kind = kind;
        g.antialias = aa;
        g.scale_x = sx;
        g.scale_y = sy;
        g.pivot = pivot;
        g.recolor_opa = recolor_opa;
        groups.push_back(g);
    };
    for(const bool aa : { false, true }) {
        for(const int32_t scale : { LV_SCALE_NONE, 300 }) {
            for(const Pivot& pivot : { CENTER, CORNER, OFF_CENTER }) {
                add(Kind::Rotate, aa, scale, scale, pivot, "rotate");
            }
        }
    }
    for(const bool aa : { false, true }) {
        add(Kind::ScaleOnly, aa, 300, 300, CENTER, "scale-only");
    }
    for(const bool aa : { false, true }) {
        add(Kind::Rotate, aa, 300, 180, OFF_CENTER, "rotate-aniso");
    }
    for(const bool aa : { false, true }) {
        add(Kind::Needle, aa, LV_SCALE_NONE, LV_SCALE_NONE, NEEDLE_PIVOT, "needle");
    }
    // Several chunks and an x range clipped by the canvas.
    add(Kind::Rotate, true, 768, 768, OFF_CENTER, "rotate-big");
    // The recolor step of the chunk loop, with a translucent recolor.
    add(Kind::Rotate, true, 300, 300, OFF_CENTER, "rotate-recolor", LV_OPA_50);
    return groups;
}

// clang-format off: tables, one row per format, one column per group (Groups() order: rotate aa=0 scale 256
// center, corner, off-center, scale 300 the same, aa=1 the same, scale-only aa=0/1, rotate-aniso aa=0/1, needle aa=0/1,
// rotate-big, rotate-recolor). GOLDEN holds the ARGB8888 target, GOLDEN_RGB565 the RGB565 target.
struct GoldenRow {
    const char* format;
    uint32_t crc[GROUPS_PER_FORMAT];
};
const GoldenRow GOLDEN[] = {
    { "RGB565A8", {
        0x7a8c87a9, 0x605162eb, 0x45916f43, 0x92ffe855, 0x2bacb593, 0x293b8b46,
        0xd42857d3, 0xa3c46328, 0x7256a240, 0x6f0d42f7, 0xc102cac5, 0x68a0c126,
        0x27bacc56, 0xebff615b, 0x9668e888, 0x5374444e, 0xe753455d, 0x42b6fc2c,
        0xb3ea1d55, 0x91fe06d9,
    } },
    { "RGB565", {
        0x2c1bf64c, 0xe7fc588c, 0x40b772ca, 0x30c8e54d, 0xe951e9f8, 0xae9452e3,
        0x32f13532, 0xb2690938, 0x35c48038, 0x420c9ba4, 0xc367d620, 0x9dd4d66f,
        0x1274b041, 0x88429325, 0x7a987f10, 0x8ce55209, 0x67405028, 0x59ce2799,
        0x307ff99a, 0x293c6e73,
    } },
    { "RGB565_SWAPPED", {
        0x2c1bf64c, 0xe7fc588c, 0x40b772ca, 0x30c8e54d, 0xe951e9f8, 0xae9452e3,
        0x32f13532, 0xb2690938, 0x35c48038, 0x420c9ba4, 0xc367d620, 0x9dd4d66f,
        0x1274b041, 0x88429325, 0x7a987f10, 0x8ce55209, 0x67405028, 0x59ce2799,
        0x307ff99a, 0x293c6e73,
    } },
    { "ARGB8888", {
        0xfead91df, 0x76d72cc6, 0xe25b1d99, 0x1e4617a5, 0x6ad95b53, 0x31564117,
        0x0a0ed24e, 0xe8e2cd17, 0xc8f73b30, 0x3be4a7fd, 0x9e63a39b, 0xacbfaa79,
        0xee4c6e88, 0x7b4c2b40, 0x388fd488, 0xd1d8ebe4, 0x36a4aa74, 0xdced2039,
        0xd181552b, 0x29b43ba0,
    } },
    { "ARGB8888_PREMULTIPLIED", {
        0x83b0b0f5, 0x0aea023c, 0x227111b5, 0x8ce32e14, 0xd1ae7461, 0xbd7dc3c7,
        0xb2eaa4ee, 0xddf2a916, 0x28e668ea, 0x05c48913, 0xad220437, 0x992a53bc,
        0x9b2faa56, 0xf560c01a, 0x546a7b8f, 0x07574624, 0xb796e538, 0xd616a618,
        0x23b9a094, 0x550309bf,
    } },
    { "RGB888", {
        0x192266d9, 0x90873733, 0x67c9ed2e, 0x969bca00, 0xa2783307, 0x9a2bb295,
        0xd2b4d32d, 0x4c8c0745, 0xd986f71d, 0xfd093971, 0x3c2a7994, 0x51b98883,
        0xf6ef41cc, 0x7626306b, 0x0106f9e5, 0xbe784042, 0x03297632, 0xb19f34da,
        0xde2b72fc, 0x92f2e6d7,
    } },
    { "XRGB8888", {
        0x192266d9, 0x90873733, 0x67c9ed2e, 0x969bca00, 0xa2783307, 0x9a2bb295,
        0xd2b4d32d, 0x4c8c0745, 0xd986f71d, 0xfd093971, 0x3c2a7994, 0x51b98883,
        0xf6ef41cc, 0x7626306b, 0x0106f9e5, 0xbe784042, 0x03297632, 0xb19f34da,
        0xde2b72fc, 0x92f2e6d7,
    } },
    { "A8", {
        0x18841b03, 0x34e4c620, 0x97125627, 0xf0ac4ce6, 0x6814aff0, 0x3798aa10,
        0x95c3b62c, 0x6fb86aca, 0x1a518ee6, 0x30853464, 0x1a1ecf32, 0x357c63ad,
        0x205f8811, 0xc6069cd3, 0x2fb1594b, 0xb20158a2, 0xedad48e6, 0xc0b9519e,
        0x55e8caf6, 0x357c63ad,
    } },
    { "L8", {
        0x3564b12a, 0x3c6dc184, 0x21e69375, 0xc543f10f, 0x987a970e, 0x4e4cd6da,
        0xa7b2c339, 0x13554d3e, 0x9663f057, 0x5eccc969, 0xa9745e2d, 0xad83e30c,
        0xa77dbaa6, 0x3e832de0, 0xb81866e6, 0xb08673fb, 0x6f7b925c, 0x5c9404e7,
        0xe380cfbb, 0xad83e30c,
    } },
    { "AL88", {
        0xfbe69886, 0x0b0d9071, 0x0ca791bb, 0x535932bd, 0xcdd095f1, 0xd6f9ba6b,
        0xfb8ccd0b, 0xb485aa57, 0x232de5be, 0xaff1fa24, 0xacff501a, 0x43f85274,
        0x39fec607, 0x59b56c8e, 0x6a01ccec, 0x7e13d540, 0xd59c03e6, 0x92aac9da,
        0x84417f07, 0x43f85274,
    } },
};
const GoldenRow GOLDEN_RGB565[] = {
    { "RGB565A8", {
        0xe6cdc18b, 0x13f9cc2c, 0x1b9770d8, 0xab9d45f3, 0xcb4e2f0a, 0x9cfc525f,
        0x4ed7bb87, 0xa37341fe, 0xab7b9923, 0xab3501ce, 0xa2c2409c, 0xa5cda1ef,
        0x76e4aee1, 0x42d96a5b, 0x8332c03f, 0x2c98433e, 0x07da7e00, 0xfcf4e3cd,
        0xcd1472ff, 0x67359383,
    } },
    { "RGB565", {
        0x786adbbb, 0x60582839, 0xd337cf01, 0x32562c5f, 0x9ad5509d, 0x1614cf9c,
        0x23b59b12, 0xef378d0b, 0x86157c0c, 0xbae468a7, 0xd394f6f2, 0xd1b89487,
        0x69600e22, 0x26ed2685, 0xc97e38d8, 0xcbcabe86, 0x821e6ff9, 0xb42f7b59,
        0xa7edfb60, 0x759cfec0,
    } },
    { "RGB565_SWAPPED", {
        0x786adbbb, 0x60582839, 0xd337cf01, 0x32562c5f, 0x9ad5509d, 0x1614cf9c,
        0x23b59b12, 0xef378d0b, 0x86157c0c, 0xbae468a7, 0xd394f6f2, 0xd1b89487,
        0x69600e22, 0x26ed2685, 0xc97e38d8, 0xcbcabe86, 0x821e6ff9, 0xb42f7b59,
        0xa7edfb60, 0x759cfec0,
    } },
    { "ARGB8888", {
        0xe6cdc18b, 0x13f9cc2c, 0x1b9770d8, 0xab9d45f3, 0xcb4e2f0a, 0x9cfc525f,
        0xf50ab241, 0x309d16e6, 0xe7523b8b, 0x7f044100, 0x49de7d62, 0xcd678254,
        0x76e4aee1, 0xb19c939a, 0x8332c03f, 0x2f6ad713, 0x07da7e00, 0x701a0dd2,
        0xa2844e44, 0x2ce2e9d2,
    } },
    { "ARGB8888_PREMULTIPLIED", {
        0xb3971879, 0x2b42078b, 0xbaffc2d6, 0xa7fde046, 0x1dbf27af, 0x141e2c82,
        0xb2050ed9, 0x7eacc502, 0x1098cabc, 0x0dd85ad1, 0x87e8ca66, 0x8c910468,
        0x191b72dc, 0xce052cc0, 0xf200217b, 0x423bba28, 0xa14baa8c, 0x9e507cc7,
        0xec7293b3, 0xeea21e47,
    } },
    { "RGB888", {
        0x786adbbb, 0x60582839, 0xd337cf01, 0x32562c5f, 0x9ad5509d, 0x1614cf9c,
        0xce6276fd, 0x5e02e293, 0xb5256946, 0x275a00d5, 0x22e34b1e, 0xc02afbfc,
        0x69600e22, 0x7ab089f8, 0xc97e38d8, 0xa55721e8, 0x821e6ff9, 0x91e3ecaf,
        0xbd7edb92, 0x0dce01b1,
    } },
    { "XRGB8888", {
        0x786adbbb, 0x60582839, 0xd337cf01, 0x32562c5f, 0x9ad5509d, 0x1614cf9c,
        0xce6276fd, 0x5e02e293, 0xb5256946, 0x275a00d5, 0x22e34b1e, 0xc02afbfc,
        0x69600e22, 0x7ab089f8, 0xc97e38d8, 0xa55721e8, 0x821e6ff9, 0x91e3ecaf,
        0xbd7edb92, 0x0dce01b1,
    } },
    { "A8", {
        0xa8b61845, 0x64e4a647, 0x2d034aa1, 0x67f0a261, 0x71c037f9, 0x870366b6,
        0x52c26034, 0x2468f62c, 0xc29c68b7, 0xc98c8379, 0x636ee376, 0x566d672a,
        0x192d30ea, 0x5597cf71, 0xa0ac838f, 0x5e395231, 0x42d7a62d, 0xdc3c78a1,
        0x657666a4, 0x566d672a,
    } },
    { "L8", {
        0xdccbfe81, 0x8fd570d9, 0x1635d534, 0x92068e64, 0x3d0d3b02, 0x0a29ac83,
        0xe677f7d1, 0xe9797a3f, 0xd7d7204b, 0xca5e6b5e, 0x0da97ba0, 0x08ab04eb,
        0x70cd8ff6, 0x2c608ceb, 0x8d9a0321, 0x3516252f, 0x41a57bcb, 0x69f2b96c,
        0xbfb07d42, 0x08ab04eb,
    } },
    { "AL88", {
        0x004a7882, 0x2765b0b8, 0x94950dc5, 0x2047b7bb, 0xbd95dee5, 0x24246383,
        0x8de3b2f2, 0x571bfd92, 0xd460a5dc, 0x4715fa84, 0xddb56ce4, 0x6c681993,
        0x24e29081, 0x424ff6ac, 0x9f7e80af, 0x4d176c22, 0xf47f6fd6, 0xa2a4b9ea,
        0x3dd3ce7e, 0x6c681993,
    } },
};
// clang-format on

struct Target {
    const char* table;
    lv_color_format_t cf;
    uint32_t px_size;
    const GoldenRow* rows;
    size_t row_count;
};

// clang-format off: a table.
const Target TARGETS[] = {
    { "GOLDEN", LV_COLOR_FORMAT_ARGB8888, 4, GOLDEN, sizeof(GOLDEN) / sizeof(GOLDEN[0]) },
    { "GOLDEN_RGB565", LV_COLOR_FORMAT_RGB565, 2, GOLDEN_RGB565, sizeof(GOLDEN_RGB565) / sizeof(GOLDEN_RGB565[0]) },
};
// clang-format on

const GoldenRow* FindRow(const Target& target, const char* format) {
    for(size_t i = 0; i < target.row_count; ++i) {
        if(std::strcmp(target.rows[i].format, format) == 0) {
            return &target.rows[i];
        }
    }
    return nullptr;
}

// A 32-bit digest of the canvas: FNV-1a over its 32-bit words. A CRC32 over every byte of every canvas costs
// several seconds of emulated time per format on qemu_cortex_a9, the digest a tenth of that.
uint32_t Digest(const lv_draw_buf_t* buf) {
    const uint8_t* bytes = buf->data;
    const size_t count = static_cast<size_t>(buf->header.stride) * buf->header.h / sizeof(uint32_t);
    uint32_t hash = 2166136261U;
    for(size_t i = 0; i < count; ++i) {
        uint32_t word = 0;
        std::memcpy(&word, bytes + i * sizeof(word), sizeof(word));
        hash = (hash ^ word) * 16777619U;
    }
    return hash;
}

// The CRC32 of the digests of the canvas after each draw of the group, chained.
uint32_t DrawGroup(
    const Format& format,
    const Group& group,
    const Source& source,
    lv_draw_buf_t* buf,
    const lv_draw_buf_t* background,
    lv_obj_t* canvas,
    int32_t size
) {
    std::vector<int32_t> angles{ 0 };
    if(group.kind == Kind::Rotate) {
        angles = Angles(group.scale_x != group.scale_y ? 14 : 7);
    } else if(group.kind == Kind::Needle) {
        angles = Angles(30);
    }
    const int32_t w = static_cast<int32_t>(source.Dsc()->header.w);
    const int32_t h = static_cast<int32_t>(source.Dsc()->header.h);
    const int32_t anchor = size / 2;
    uint32_t crc = 0;
    // Scale-only draws: one run per scale, so the group covers growing and shrinking.
    const std::vector<int32_t> scales = group.kind == Kind::ScaleOnly ? std::vector<int32_t>{ 300, 128, 77 }
                                                                      : std::vector<int32_t>{ group.scale_x };

    for(const int32_t scale : scales) {
        for(const int32_t angle : angles) {
            std::memcpy(buf->data, background->data, background->header.stride * background->header.h);

            lv_layer_t layer;
            lv_canvas_init_layer(canvas, &layer);
            lv_draw_image_dsc_t dsc;
            lv_draw_image_dsc_init(&dsc);
            dsc.src = source.Dsc();
            dsc.rotation = angle;
            dsc.scale_x = scale;
            dsc.scale_y = group.kind == Kind::ScaleOnly ? scale : group.scale_y;
            dsc.pivot.x = group.pivot.x;
            dsc.pivot.y = group.pivot.y;
            dsc.antialias = group.antialias ? 1 : 0;
            if(group.recolor_opa != 0) {
                dsc.recolor = lv_color_make(0xE0, 0x90, 0x20);
                dsc.recolor_opa = group.recolor_opa;
            } else if(format.recolor) {
                dsc.recolor = lv_color_make(0xE0, 0x90, 0x20);
                dsc.recolor_opa = LV_OPA_COVER;
            }
            const int32_t x1 = anchor - group.pivot.x;
            const int32_t y1 = anchor - group.pivot.y;
            const lv_area_t area = { x1, y1, x1 + w - 1, y1 + h - 1 };
            lv_draw_image(&layer, &dsc, &area);
            lv_canvas_finish_layer(canvas, &layer);

            const uint32_t digest = Digest(buf);
            uint8_t digest_bytes[sizeof(digest)];
            std::memcpy(digest_bytes, &digest, sizeof(digest));
            crc = crc32_ieee_update(crc, digest_bytes, sizeof(digest_bytes));
        }
    }
    return crc;
}

// Static pixel storage: the heap of the suite is too small for the needle canvases.
alignas(64) uint8_t canvas_pixels[NEEDLE_CANVAS_SIZE * NEEDLE_CANVAS_SIZE * 4];
alignas(64) uint8_t background_pixels[NEEDLE_CANVAS_SIZE * NEEDLE_CANVAS_SIZE * 4];
lv_draw_buf_t canvas_buf;
lv_draw_buf_t background_buf;

void RunFormat(size_t index, const Target& target) {
    views_test::EnsureTestDisplay();
    const Format& format = FORMATS[index];
    const std::vector<Group> groups = Groups();
    zassert_equal(groups.size(), GROUPS_PER_FORMAT);

    const Source image(format.cf, IMAGE_W, IMAGE_H);
    const Source needle(format.cf, NEEDLE_W, NEEDLE_H);

    uint32_t crcs[GROUPS_PER_FORMAT] = {};
    for(const int32_t size : { CANVAS_SIZE, NEEDLE_CANVAS_SIZE }) {
        lv_draw_buf_t* buf = &canvas_buf;
        lv_draw_buf_t* background = &background_buf;
        const uint32_t stride = static_cast<uint32_t>(size) * target.px_size;
        zassert_equal(
            lv_draw_buf_init(buf, size, size, target.cf, stride, canvas_pixels, sizeof(canvas_pixels)),
            LV_RESULT_OK
        );
        zassert_equal(
            lv_draw_buf_init(background, size, size, target.cf, stride, background_pixels, sizeof(background_pixels)),
            LV_RESULT_OK
        );
        lv_obj_t* canvas = lv_canvas_create(lv_screen_active());
        lv_canvas_set_draw_buf(canvas, background);
        lv_canvas_fill_bg(canvas, lv_color_hex(BACKGROUND & 0xFFFFFF), LV_OPA_COVER);
        lv_canvas_set_draw_buf(canvas, buf);

        for(size_t g = 0; g < groups.size(); ++g) {
            const bool is_needle = groups[g].kind == Kind::Needle;
            if(is_needle != (size == NEEDLE_CANVAS_SIZE)) {
                continue;
            }
            crcs[g] = DrawGroup(format, groups[g], is_needle ? needle : image, buf, background, canvas, size);
        }
        lv_obj_delete(canvas);
    }

    if constexpr(REGENERATE) {
        printk("%s_TABLE_BEGIN\n    { \"%s\", {", target.table, format.name);
        for(size_t g = 0; g < GROUPS_PER_FORMAT; ++g) {
            printk("%s0x%08x,", g % 6 == 0 ? "\n        " : " ", static_cast<unsigned>(crcs[g]));
        }
        printk("\n    } },\n%s_TABLE_END\n", target.table);
        return;
    }

    const GoldenRow* row = FindRow(target, format.name);
    zassert_not_null(row, "no %s row for format %s", target.table, format.name);
    bool all_match = true;
    for(size_t g = 0; g < groups.size(); ++g) {
        if(crcs[g] != row->crc[g]) {
            all_match = false;
            printk(
                "MISMATCH %s, %s, %s: crc 0x%08x, expected 0x%08x\n", target.table, format.name, groups[g].name,
                static_cast<unsigned>(crcs[g]), static_cast<unsigned>(row->crc[g])
            );
        }
    }
    zassert_true(all_match, "image transform output of %s differs from the table %s", format.name, target.table);
}

} // namespace

#define FORMAT_TEST(INDEX, NAME) \
    ZTEST(image_transform_golden, test_##NAME) { \
        if((INDEX) < FORMAT_COUNT) { \
            for(const Target& target : TARGETS) { \
                RunFormat((INDEX), target); \
            } \
        } \
    }

FORMAT_TEST(0, format_0)
FORMAT_TEST(1, format_1)
FORMAT_TEST(2, format_2)
FORMAT_TEST(3, format_3)
FORMAT_TEST(4, format_4)
FORMAT_TEST(5, format_5)
FORMAT_TEST(6, format_6)
FORMAT_TEST(7, format_7)
FORMAT_TEST(8, format_8)
FORMAT_TEST(9, format_9)

static_assert(FORMAT_COUNT <= 10, "add a FORMAT_TEST per format");

ZTEST_SUITE(image_transform_golden, nullptr, nullptr, nullptr, views_test::CleanTestDisplay, nullptr);
