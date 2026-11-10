#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <lvgl.h>
#include <zephyr/ztest.h>

#include "domain/ui_domain/images/image_file.h"

using namespace eerie_leap::domain::ui_domain::images;

namespace {

constexpr uint32_t MAX_PIXEL_BYTES = 4194304;

constexpr std::array COLOR_16_FORMATS = { LV_COLOR_FORMAT_RGB565, LV_COLOR_FORMAT_RGB565A8 };
constexpr std::array COLOR_8_FORMATS = { LV_COLOR_FORMAT_L8, LV_COLOR_FORMAT_AL88 };
constexpr std::array LZ4_ONLY = { LV_IMAGE_COMPRESS_LZ4 };

const ImageFileRules COLOR_16{ .formats = COLOR_16_FORMATS, .compressions = {}, .max_pixel_bytes = MAX_PIXEL_BYTES };
const ImageFileRules COLOR_8{ .formats = COLOR_8_FORMATS, .compressions = {}, .max_pixel_bytes = MAX_PIXEL_BYTES };
const ImageFileRules COLOR_16_LZ4{ .formats = COLOR_16_FORMATS,
    .compressions = LZ4_ONLY,
    .max_pixel_bytes = MAX_PIXEL_BYTES };
const ImageFileRules COLOR_8_LZ4{ .formats = COLOR_8_FORMATS,
    .compressions = LZ4_ONLY,
    .max_pixel_bytes = MAX_PIXEL_BYTES };

constexpr uint8_t L8 = LV_COLOR_FORMAT_L8;
constexpr uint8_t RGB565 = LV_COLOR_FORMAT_RGB565;
constexpr uint8_t RGB565A8 = LV_COLOR_FORMAT_RGB565A8;
constexpr uint8_t AL88 = LV_COLOR_FORMAT_AL88;

constexpr uint16_t COMPRESSED = LV_IMAGE_FLAGS_COMPRESSED;
constexpr uint32_t LZ4_METHOD = LV_IMAGE_COMPRESS_LZ4;

struct HeaderFields {
    uint8_t magic = LV_IMAGE_HEADER_MAGIC;
    uint8_t cf = RGB565;
    uint16_t flags = 0;
    uint16_t w = 4;
    uint16_t h = 3;
    uint16_t stride = 8;
    uint16_t reserved = 0;
};

void PutU16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xFF));
    bytes.push_back(static_cast<uint8_t>(value >> 8));
}

void PutU32(std::vector<uint8_t>& bytes, uint32_t value) {
    PutU16(bytes, static_cast<uint16_t>(value & 0xFFFF));
    PutU16(bytes, static_cast<uint16_t>(value >> 16));
}

/// The 12 bytes of an image header, little-endian as a file holds them.
std::vector<uint8_t> Header(const HeaderFields& fields) {
    std::vector<uint8_t> bytes;
    bytes.push_back(fields.magic);
    bytes.push_back(fields.cf);
    PutU16(bytes, fields.flags);
    PutU16(bytes, fields.w);
    PutU16(bytes, fields.h);
    PutU16(bytes, fields.stride);
    PutU16(bytes, fields.reserved);
    return bytes;
}

/// A compressed file's 24 bytes of heads: the image header, then LVGL's compressed-data header.
std::vector<uint8_t>
CompressedHead(HeaderFields fields, uint32_t method, uint32_t compressed_bytes, uint32_t decompressed_bytes) {
    fields.flags = COMPRESSED;
    auto bytes = Header(fields);
    PutU32(bytes, method);
    PutU32(bytes, compressed_bytes);
    PutU32(bytes, decompressed_bytes);
    return bytes;
}

std::expected<ImageFileHead, ImageFileError>
Parse(const std::vector<uint8_t>& head, size_t file_length, const ImageFileRules& rules) {
    return ParseImageFileHead(head, file_length, rules);
}

/// A file with a valid head of @p fields and @p pixel_bytes after it: the length that makes it a whole file.
std::expected<ImageFileHead, ImageFileError>
ParseWhole(const HeaderFields& fields, size_t pixel_bytes, const ImageFileRules& rules) {
    return Parse(Header(fields), 12 + pixel_bytes, rules);
}

void ExpectFailure(
    const std::expected<ImageFileHead, ImageFileError>& result,
    ImageFileProblem problem,
    const char* message
) {
    zassert_false(result.has_value(), "The file was taken: %s", message);
    zassert_equal(
        static_cast<int>(result.error().problem), static_cast<int>(problem), "%s", result.error().message.c_str()
    );
    zassert_equal(result.error().message, std::string(message));
}

// Files made by tools/LVGLImage.py from three 8 x 6 pictures.
const std::array<uint8_t, 108> OPAQUE_RGB565 = { 0x19, 0x12, 0x00, 0x00, 0x08, 0x00, 0x06, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x02, 0x20, 0x05, 0x40, 0x07, 0x68, 0x0A, 0x90, 0x0D, 0xB0, 0x0F, 0xD8, 0x12, 0xF8, 0x82, 0x01, 0x85,
    0x21, 0x87, 0x41, 0x8A, 0x69, 0x8D, 0x91, 0x8F, 0xB1, 0x92, 0xD9, 0x95, 0xF9, 0x25, 0x03, 0x27, 0x23, 0x2A, 0x43,
    0x2D, 0x6B, 0x2F, 0x93, 0x32, 0xB3, 0x35, 0xDB, 0x37, 0xFB, 0xC7, 0x04, 0xCA, 0x24, 0xCD, 0x44, 0xCF, 0x6C, 0xD2,
    0x94, 0xD5, 0xB4, 0xD7, 0xDC, 0xDA, 0xFC, 0x4A, 0x06, 0x4D, 0x26, 0x4F, 0x46, 0x52, 0x6E, 0x55, 0x96, 0x57, 0xB6,
    0x5A, 0xDE, 0x5D, 0xFE, 0xED, 0x07, 0xEF, 0x27, 0xF2, 0x47, 0xF5, 0x6F, 0xF7, 0x97, 0xFA, 0xB7, 0xFD, 0xDF, 0xFF,
    0xFF };

const std::array<uint8_t, 156> OPAQUE_RGB565A8 = { 0x19, 0x14, 0x00, 0x00, 0x08, 0x00, 0x06, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x20, 0x05, 0x48, 0x07, 0x68, 0x0A, 0x90, 0x0D, 0xB0, 0x0F, 0xD8, 0x12, 0xF8, 0x82, 0x01,
    0x85, 0x21, 0x87, 0x49, 0x8A, 0x69, 0x8D, 0x91, 0x8F, 0xB1, 0x92, 0xD9, 0x95, 0xF9, 0x25, 0x03, 0x27, 0x23, 0x2A,
    0x4B, 0x2D, 0x6B, 0x2F, 0x93, 0x32, 0xB3, 0x35, 0xDB, 0x37, 0xFB, 0xC7, 0x04, 0xCA, 0x24, 0xCD, 0x4C, 0xCF, 0x6C,
    0xD2, 0x94, 0xD5, 0xB4, 0xD7, 0xDC, 0xDA, 0xFC, 0x6A, 0x06, 0x6D, 0x26, 0x6F, 0x4E, 0x72, 0x6E, 0x75, 0x96, 0x77,
    0xB6, 0x7A, 0xDE, 0x7D, 0xFE, 0xED, 0x07, 0xEF, 0x27, 0xF2, 0x4F, 0xF5, 0x6F, 0xF7, 0x97, 0xFA, 0xB7, 0xFD, 0xDF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

const std::array<uint8_t, 60> OPAQUE_L8 = { 0x19, 0x06, 0x00, 0x00, 0x08, 0x00, 0x06, 0x00, 0x08, 0x00, 0x00, 0x00,
    0x00, 0x0D, 0x20, 0x34, 0x48, 0x5C, 0x70, 0x84, 0x2A, 0x2E, 0x38, 0x45, 0x55, 0x67, 0x79, 0x8B, 0x56, 0x59, 0x5E,
    0x66, 0x71, 0x7F, 0x8D, 0x9D, 0x83, 0x84, 0x88, 0x8E, 0x95, 0x9F, 0xAB, 0xB8, 0xAF, 0xB1, 0xB3, 0xB8, 0xBE, 0xC5,
    0xCE, 0xD9, 0xDC, 0xDD, 0xDF, 0xE3, 0xE7, 0xED, 0xF5, 0xFD };

const std::array<uint8_t, 108> OPAQUE_AL88 = { 0x19, 0x15, 0x00, 0x00, 0x08, 0x00, 0x06, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x00, 0xFF, 0x0D, 0xFF, 0x21, 0xFF, 0x35, 0xFF, 0x49, 0xFF, 0x5D, 0xFF, 0x70, 0xFF, 0x84, 0xFF, 0x2B, 0xFF, 0x2F,
    0xFF, 0x39, 0xFF, 0x46, 0xFF, 0x56, 0xFF, 0x68, 0xFF, 0x79, 0xFF, 0x8C, 0xFF, 0x57, 0xFF, 0x5A, 0xFF, 0x5F, 0xFF,
    0x67, 0xFF, 0x72, 0xFF, 0x7F, 0xFF, 0x8E, 0xFF, 0x9E, 0xFF, 0x84, 0xFF, 0x85, 0xFF, 0x89, 0xFF, 0x8F, 0xFF, 0x96,
    0xFF, 0xA0, 0xFF, 0xAC, 0xFF, 0xB9, 0xFF, 0xB0, 0xFF, 0xB2, 0xFF, 0xB4, 0xFF, 0xB9, 0xFF, 0xBF, 0xFF, 0xC6, 0xFF,
    0xCF, 0xFF, 0xDA, 0xFF, 0xDD, 0xFF, 0xDE, 0xFF, 0xE0, 0xFF, 0xE4, 0xFF, 0xE8, 0xFF, 0xEE, 0xFF, 0xF6, 0xFF, 0xFE,
    0xFF };

const std::array<uint8_t, 156> ALPHA_RGB565A8 = { 0x19, 0x14, 0x00, 0x00, 0x08, 0x00, 0x06, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x20, 0x05, 0x48, 0x07, 0x68, 0x0A, 0x90, 0x0D, 0xB0, 0x0F, 0xD8, 0x12, 0xF8, 0x82, 0x01,
    0x85, 0x21, 0x87, 0x49, 0x8A, 0x69, 0x8D, 0x91, 0x8F, 0xB1, 0x92, 0xD9, 0x95, 0xF9, 0x25, 0x03, 0x27, 0x23, 0x2A,
    0x4B, 0x2D, 0x6B, 0x2F, 0x93, 0x32, 0xB3, 0x35, 0xDB, 0x37, 0xFB, 0xC7, 0x04, 0xCA, 0x24, 0xCD, 0x4C, 0xCF, 0x6C,
    0xD2, 0x94, 0xD5, 0xB4, 0xD7, 0xDC, 0xDA, 0xFC, 0x6A, 0x06, 0x6D, 0x26, 0x6F, 0x4E, 0x72, 0x6E, 0x75, 0x96, 0x77,
    0xB6, 0x7A, 0xDE, 0x7D, 0xFE, 0xED, 0x07, 0xEF, 0x27, 0xF2, 0x4F, 0xF5, 0x6F, 0xF7, 0x97, 0xFA, 0xB7, 0xFD, 0xDF,
    0xFF, 0xFF, 0x00, 0x05, 0x0A, 0x10, 0x15, 0x1B, 0x20, 0x25, 0x2B, 0x30, 0x36, 0x3B, 0x41, 0x46, 0x4B, 0x51, 0x56,
    0x5C, 0x61, 0x67, 0x6C, 0x71, 0x77, 0x7C, 0x82, 0x87, 0x8D, 0x92, 0x97, 0x9D, 0xA2, 0xA8, 0xAD, 0xB3, 0xB8, 0xBD,
    0xC3, 0xC8, 0xCE, 0xD3, 0xD9, 0xDE, 0xE3, 0xE9, 0xEE, 0xF4, 0xF9, 0xFF };

const std::array<uint8_t, 108> ALPHA_AL88 = { 0x19, 0x15, 0x00, 0x00, 0x08, 0x00, 0x06, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x0D, 0x05, 0x21, 0x0A, 0x35, 0x10, 0x49, 0x15, 0x5D, 0x1B, 0x70, 0x20, 0x84, 0x25, 0x2B, 0x2B, 0x2F,
    0x30, 0x39, 0x36, 0x46, 0x3B, 0x56, 0x41, 0x68, 0x46, 0x79, 0x4B, 0x8C, 0x51, 0x57, 0x56, 0x5A, 0x5C, 0x5F, 0x61,
    0x67, 0x67, 0x72, 0x6C, 0x7F, 0x71, 0x8E, 0x77, 0x9E, 0x7C, 0x84, 0x82, 0x85, 0x87, 0x89, 0x8D, 0x8F, 0x92, 0x96,
    0x97, 0xA0, 0x9D, 0xAC, 0xA2, 0xB9, 0xA8, 0xB0, 0xAD, 0xB2, 0xB3, 0xB4, 0xB8, 0xB9, 0xBD, 0xBF, 0xC3, 0xC6, 0xC8,
    0xCF, 0xCE, 0xDA, 0xD3, 0xDD, 0xD9, 0xDE, 0xDE, 0xE0, 0xE3, 0xE4, 0xE9, 0xE8, 0xEE, 0xEE, 0xF4, 0xF6, 0xF9, 0xFE,
    0xFF };

const std::array<uint8_t, 156> GREY_RGB565A8 = { 0x19, 0x14, 0x00, 0x00, 0x08, 0x00, 0x06, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x24, 0x21, 0x49, 0x4A, 0x6D, 0x6B, 0x92, 0x94, 0xB6, 0xB5, 0xDB, 0xDE, 0xFF, 0xFF, 0x00, 0x00, 0x24,
    0x21, 0x49, 0x4A, 0x6D, 0x6B, 0x92, 0x94, 0xB6, 0xB5, 0xDB, 0xDE, 0xFF, 0xFF, 0x00, 0x00, 0x24, 0x21, 0x49, 0x4A,
    0x6D, 0x6B, 0x92, 0x94, 0xB6, 0xB5, 0xDB, 0xDE, 0xFF, 0xFF, 0x00, 0x00, 0x24, 0x21, 0x49, 0x4A, 0x6D, 0x6B, 0x92,
    0x94, 0xB6, 0xB5, 0xDB, 0xDE, 0xFF, 0xFF, 0x00, 0x00, 0x24, 0x21, 0x49, 0x4A, 0x6D, 0x6B, 0x92, 0x94, 0xB6, 0xB5,
    0xDB, 0xDE, 0xFF, 0xFF, 0x00, 0x00, 0x24, 0x21, 0x49, 0x4A, 0x6D, 0x6B, 0x92, 0x94, 0xB6, 0xB5, 0xDB, 0xDE, 0xFF,
    0xFF, 0x00, 0x05, 0x0A, 0x10, 0x15, 0x1B, 0x20, 0x25, 0x2B, 0x30, 0x36, 0x3B, 0x41, 0x46, 0x4B, 0x51, 0x56, 0x5C,
    0x61, 0x67, 0x6C, 0x71, 0x77, 0x7C, 0x82, 0x87, 0x8D, 0x92, 0x97, 0x9D, 0xA2, 0xA8, 0xAD, 0xB3, 0xB8, 0xBD, 0xC3,
    0xC8, 0xCE, 0xD3, 0xD9, 0xDE, 0xE3, 0xE9, 0xEE, 0xF4, 0xF9, 0xFF };

const std::array<uint8_t, 108> GREY_AL88 = { 0x19, 0x15, 0x00, 0x00, 0x08, 0x00, 0x06, 0x00, 0x10, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x24, 0x05, 0x48, 0x0A, 0x6D, 0x10, 0x91, 0x15, 0xB6, 0x1B, 0xDA, 0x20, 0xFE, 0x25, 0x00, 0x2B, 0x24,
    0x30, 0x48, 0x36, 0x6D, 0x3B, 0x91, 0x41, 0xB6, 0x46, 0xDA, 0x4B, 0xFE, 0x51, 0x00, 0x56, 0x24, 0x5C, 0x48, 0x61,
    0x6D, 0x67, 0x91, 0x6C, 0xB6, 0x71, 0xDA, 0x77, 0xFE, 0x7C, 0x00, 0x82, 0x24, 0x87, 0x48, 0x8D, 0x6D, 0x92, 0x91,
    0x97, 0xB6, 0x9D, 0xDA, 0xA2, 0xFE, 0xA8, 0x00, 0xAD, 0x24, 0xB3, 0x48, 0xB8, 0x6D, 0xBD, 0x91, 0xC3, 0xB6, 0xC8,
    0xDA, 0xCE, 0xFE, 0xD3, 0x00, 0xD9, 0x24, 0xDE, 0x48, 0xE3, 0x6D, 0xE9, 0x91, 0xEE, 0xB6, 0xF4, 0xDA, 0xF9, 0xFE,
    0xFF };

const std::array<uint8_t, 204> OPAQUE_RGB565_ALIGN32 = { 0x19, 0x12, 0x00, 0x00, 0x08, 0x00, 0x06, 0x00, 0x20, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x20, 0x05, 0x40, 0x07, 0x68, 0x0A, 0x90, 0x0D, 0xB0, 0x0F, 0xD8, 0x12, 0xF8, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x82, 0x01, 0x85, 0x21,
    0x87, 0x41, 0x8A, 0x69, 0x8D, 0x91, 0x8F, 0xB1, 0x92, 0xD9, 0x95, 0xF9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25, 0x03, 0x27, 0x23, 0x2A, 0x43, 0x2D, 0x6B, 0x2F, 0x93,
    0x32, 0xB3, 0x35, 0xDB, 0x37, 0xFB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xC7, 0x04, 0xCA, 0x24, 0xCD, 0x44, 0xCF, 0x6C, 0xD2, 0x94, 0xD5, 0xB4, 0xD7, 0xDC, 0xDA, 0xFC,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4A, 0x06, 0x4D,
    0x26, 0x4F, 0x46, 0x52, 0x6E, 0x55, 0x96, 0x57, 0xB6, 0x5A, 0xDE, 0x5D, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xED, 0x07, 0xEF, 0x27, 0xF2, 0x47, 0xF5, 0x6F, 0xF7,
    0x97, 0xFA, 0xB7, 0xFD, 0xDF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00 };

const std::array<uint8_t, 300> ALPHA_RGB565A8_ALIGN32 = { 0x19, 0x14, 0x00, 0x00, 0x08, 0x00, 0x06, 0x00, 0x20, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x02, 0x20, 0x05, 0x48, 0x07, 0x68, 0x0A, 0x90, 0x0D, 0xB0, 0x0F, 0xD8, 0x12, 0xF8, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x82, 0x01, 0x85, 0x21,
    0x87, 0x49, 0x8A, 0x69, 0x8D, 0x91, 0x8F, 0xB1, 0x92, 0xD9, 0x95, 0xF9, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x25, 0x03, 0x27, 0x23, 0x2A, 0x4B, 0x2D, 0x6B, 0x2F, 0x93,
    0x32, 0xB3, 0x35, 0xDB, 0x37, 0xFB, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0xC7, 0x04, 0xCA, 0x24, 0xCD, 0x4C, 0xCF, 0x6C, 0xD2, 0x94, 0xD5, 0xB4, 0xD7, 0xDC, 0xDA, 0xFC,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x6A, 0x06, 0x6D,
    0x26, 0x6F, 0x4E, 0x72, 0x6E, 0x75, 0x96, 0x77, 0xB6, 0x7A, 0xDE, 0x7D, 0xFE, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xED, 0x07, 0xEF, 0x27, 0xF2, 0x4F, 0xF5, 0x6F, 0xF7,
    0x97, 0xFA, 0xB7, 0xFD, 0xDF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x0A, 0x10, 0x15, 0x1B, 0x20, 0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x2B, 0x30, 0x36, 0x3B, 0x41, 0x46, 0x4B, 0x51, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56, 0x5C,
    0x61, 0x67, 0x6C, 0x71, 0x77, 0x7C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x82, 0x87, 0x8D, 0x92, 0x97,
    0x9D, 0xA2, 0xA8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAD, 0xB3, 0xB8, 0xBD, 0xC3, 0xC8, 0xCE, 0xD3,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xD9, 0xDE, 0xE3, 0xE9, 0xEE, 0xF4, 0xF9, 0xFF, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00 };

const std::array<uint8_t, 108> OPAQUE_L8_ALIGN16 = { 0x19, 0x06, 0x00, 0x00, 0x08, 0x00, 0x06, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x00, 0x0D, 0x20, 0x34, 0x48, 0x5C, 0x70, 0x84, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2A, 0x2E,
    0x38, 0x45, 0x55, 0x67, 0x79, 0x8B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x56, 0x59, 0x5E, 0x66, 0x71,
    0x7F, 0x8D, 0x9D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x83, 0x84, 0x88, 0x8E, 0x95, 0x9F, 0xAB, 0xB8,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xAF, 0xB1, 0xB3, 0xB8, 0xBE, 0xC5, 0xCE, 0xD9, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0xDC, 0xDD, 0xDF, 0xE3, 0xE7, 0xED, 0xF5, 0xFD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00 };

const std::array<uint8_t, 122> OPAQUE_RGB565_LZ4 = { 0x19, 0x12, 0x08, 0x00, 0x08, 0x00, 0x06, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x62, 0x00, 0x00, 0x00, 0x60, 0x00, 0x00, 0x00, 0xF0, 0x51, 0x00, 0x00, 0x02, 0x20,
    0x05, 0x40, 0x07, 0x68, 0x0A, 0x90, 0x0D, 0xB0, 0x0F, 0xD8, 0x12, 0xF8, 0x82, 0x01, 0x85, 0x21, 0x87, 0x41, 0x8A,
    0x69, 0x8D, 0x91, 0x8F, 0xB1, 0x92, 0xD9, 0x95, 0xF9, 0x25, 0x03, 0x27, 0x23, 0x2A, 0x43, 0x2D, 0x6B, 0x2F, 0x93,
    0x32, 0xB3, 0x35, 0xDB, 0x37, 0xFB, 0xC7, 0x04, 0xCA, 0x24, 0xCD, 0x44, 0xCF, 0x6C, 0xD2, 0x94, 0xD5, 0xB4, 0xD7,
    0xDC, 0xDA, 0xFC, 0x4A, 0x06, 0x4D, 0x26, 0x4F, 0x46, 0x52, 0x6E, 0x55, 0x96, 0x57, 0xB6, 0x5A, 0xDE, 0x5D, 0xFE,
    0xED, 0x07, 0xEF, 0x27, 0xF2, 0x47, 0xF5, 0x6F, 0xF7, 0x97, 0xFA, 0xB7, 0xFD, 0xDF, 0xFF, 0xFF };

const std::array<uint8_t, 170> ALPHA_RGB565A8_LZ4 = { 0x19, 0x14, 0x08, 0x00, 0x08, 0x00, 0x06, 0x00, 0x10, 0x00, 0x00,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x92, 0x00, 0x00, 0x00, 0x90, 0x00, 0x00, 0x00, 0xF0, 0x81, 0x00, 0x00, 0x02, 0x20,
    0x05, 0x48, 0x07, 0x68, 0x0A, 0x90, 0x0D, 0xB0, 0x0F, 0xD8, 0x12, 0xF8, 0x82, 0x01, 0x85, 0x21, 0x87, 0x49, 0x8A,
    0x69, 0x8D, 0x91, 0x8F, 0xB1, 0x92, 0xD9, 0x95, 0xF9, 0x25, 0x03, 0x27, 0x23, 0x2A, 0x4B, 0x2D, 0x6B, 0x2F, 0x93,
    0x32, 0xB3, 0x35, 0xDB, 0x37, 0xFB, 0xC7, 0x04, 0xCA, 0x24, 0xCD, 0x4C, 0xCF, 0x6C, 0xD2, 0x94, 0xD5, 0xB4, 0xD7,
    0xDC, 0xDA, 0xFC, 0x6A, 0x06, 0x6D, 0x26, 0x6F, 0x4E, 0x72, 0x6E, 0x75, 0x96, 0x77, 0xB6, 0x7A, 0xDE, 0x7D, 0xFE,
    0xED, 0x07, 0xEF, 0x27, 0xF2, 0x4F, 0xF5, 0x6F, 0xF7, 0x97, 0xFA, 0xB7, 0xFD, 0xDF, 0xFF, 0xFF, 0x00, 0x05, 0x0A,
    0x10, 0x15, 0x1B, 0x20, 0x25, 0x2B, 0x30, 0x36, 0x3B, 0x41, 0x46, 0x4B, 0x51, 0x56, 0x5C, 0x61, 0x67, 0x6C, 0x71,
    0x77, 0x7C, 0x82, 0x87, 0x8D, 0x92, 0x97, 0x9D, 0xA2, 0xA8, 0xAD, 0xB3, 0xB8, 0xBD, 0xC3, 0xC8, 0xCE, 0xD3, 0xD9,
    0xDE, 0xE3, 0xE9, 0xEE, 0xF4, 0xF9, 0xFF };

struct Fixture {
    const char* name;
    std::span<const uint8_t> file;
    uint8_t cf;
    uint16_t stride;
    uint32_t pixel_bytes;
    uint32_t compressed_bytes;
};

const std::array FIXTURES = {
    Fixture{ "opaque_rgb565", OPAQUE_RGB565, RGB565, 16, 96, 0 },
    Fixture{ "opaque_rgb565a8", OPAQUE_RGB565A8, RGB565A8, 16, 144, 0 },
    Fixture{ "opaque_l8", OPAQUE_L8, L8, 8, 48, 0 },
    Fixture{ "opaque_al88", OPAQUE_AL88, AL88, 16, 96, 0 },
    Fixture{ "alpha_rgb565a8", ALPHA_RGB565A8, RGB565A8, 16, 144, 0 },
    Fixture{ "alpha_al88", ALPHA_AL88, AL88, 16, 96, 0 },
    Fixture{ "grey_rgb565a8", GREY_RGB565A8, RGB565A8, 16, 144, 0 },
    Fixture{ "grey_al88", GREY_AL88, AL88, 16, 96, 0 },
    Fixture{ "opaque_rgb565_align32", OPAQUE_RGB565_ALIGN32, RGB565, 32, 192, 0 },
    Fixture{ "alpha_rgb565a8_align32", ALPHA_RGB565A8_ALIGN32, RGB565A8, 32, 288, 0 },
    Fixture{ "opaque_l8_align16", OPAQUE_L8_ALIGN16, L8, 16, 96, 0 },
    Fixture{ "opaque_rgb565_lz4", OPAQUE_RGB565_LZ4, RGB565, 16, 96, 98 },
    Fixture{ "alpha_rgb565a8_lz4", ALPHA_RGB565A8_LZ4, RGB565A8, 16, 144, 146 },
};

bool IsColor16(uint8_t cf) {
    return cf == RGB565 || cf == RGB565A8;
}

std::span<const uint8_t> HeadOf(std::span<const uint8_t> file) {
    return file.first(std::min(file.size(), IMAGE_FILE_HEAD_SIZE));
}

} // namespace

ZTEST_SUITE(image_file, NULL, NULL, NULL, NULL, NULL);

ZTEST(image_file, test_the_extension_is_lvimg) {
    zassert_equal(IMAGE_FILE_EXTENSION, std::string_view(".lvimg"));
    zassert_equal(IMAGE_FILE_HEAD_SIZE, 24U);
}

ZTEST(image_file, test_this_builds_rules_follow_the_color_depth_and_the_kinds_cap) {
    const auto& rules = ImageFileRules::OfThisBuild();

#if LV_COLOR_DEPTH == 16
    zassert_equal(rules.formats.size(), 2U);
    zassert_equal(rules.formats[0], LV_COLOR_FORMAT_RGB565);
    zassert_equal(rules.formats[1], LV_COLOR_FORMAT_RGB565A8);
#elif LV_COLOR_DEPTH == 8
    zassert_equal(rules.formats.size(), 2U);
    zassert_equal(rules.formats[0], LV_COLOR_FORMAT_L8);
    zassert_equal(rules.formats[1], LV_COLOR_FORMAT_AL88);
#endif
    zassert_true(rules.compressions.empty());
    zassert_equal(rules.max_pixel_bytes, static_cast<uint32_t>(CONFIG_EERIE_LEAP_UI_IMAGE_MAX_SIZE));
}

// The rules, in the order they run, each with its message.

ZTEST(image_file, test_a_file_under_12_bytes_is_too_short) {
    const std::vector<uint8_t> head(11, 0x19);
    ExpectFailure(
        Parse(head, head.size(), COLOR_16), ImageFileProblem::TOO_SHORT,
        "An image file starts with a 12-byte header; this one is 11 bytes long."
    );

    ExpectFailure(
        Parse({}, 0, COLOR_16), ImageFileProblem::TOO_SHORT,
        "An image file starts with a 12-byte header; this one is 0 bytes long."
    );
}

ZTEST(image_file, test_a_file_that_does_not_start_with_the_magic_is_not_an_image) {
    ExpectFailure(
        ParseWhole({ .magic = 0x00 }, 24, COLOR_16), ImageFileProblem::NOT_AN_IMAGE,
        "Not an LVGL image file: it starts with 0x00, not 0x19."
    );

    ExpectFailure(
        ParseWhole({ .magic = 0x1D }, 24, COLOR_16), ImageFileProblem::NOT_AN_IMAGE,
        "Not an LVGL image file: it starts with 0x1D, not 0x19."
    );
}

ZTEST(image_file, test_a_color_format_the_build_does_not_draw_is_refused_and_the_message_lists_what_it_draws) {
    ExpectFailure(
        ParseWhole({ .cf = L8, .stride = 4 }, 12, COLOR_16), ImageFileProblem::FORMAT,
        "Colour format 0x06 is not one this unit draws (it draws 0x12 RGB565, 0x14 RGB565A8)."
    );

    ExpectFailure(
        ParseWhole({ .cf = AL88 }, 24, COLOR_16), ImageFileProblem::FORMAT,
        "Colour format 0x15 is not one this unit draws (it draws 0x12 RGB565, 0x14 RGB565A8)."
    );

    ExpectFailure(
        ParseWhole({ .cf = RGB565 }, 24, COLOR_8), ImageFileProblem::FORMAT,
        "Colour format 0x12 is not one this unit draws (it draws 0x06 L8, 0x15 AL88)."
    );

    ExpectFailure(
        ParseWhole({ .cf = RGB565A8 }, 36, COLOR_8), ImageFileProblem::FORMAT,
        "Colour format 0x14 is not one this unit draws (it draws 0x06 L8, 0x15 AL88)."
    );
}

ZTEST(image_file, test_a_color_format_lvgl_has_but_the_parser_has_no_facts_for_is_refused_even_when_listed) {
    constexpr std::array ARGB_ONLY = { LV_COLOR_FORMAT_ARGB8888 };
    const ImageFileRules rules{ .formats = ARGB_ONLY, .compressions = {}, .max_pixel_bytes = MAX_PIXEL_BYTES };

    ExpectFailure(
        ParseWhole({ .cf = LV_COLOR_FORMAT_ARGB8888 }, 48, rules), ImageFileProblem::FORMAT,
        "Colour format 0x10 is not one this unit draws (it draws 0x10 ?)."
    );
}

ZTEST(image_file, test_any_flag_but_compressed_is_refused) {
    ExpectFailure(
        ParseWhole({ .flags = LV_IMAGE_FLAGS_PREMULTIPLIED }, 24, COLOR_16), ImageFileProblem::FLAGS,
        "The header's flags 0x0001 are not allowed in an image file."
    );

    ExpectFailure(
        ParseWhole({ .flags = LV_IMAGE_FLAGS_ALLOCATED }, 24, COLOR_16), ImageFileProblem::FLAGS,
        "The header's flags 0x0010 are not allowed in an image file."
    );

    ExpectFailure(
        ParseWhole({ .flags = LV_IMAGE_FLAGS_MODIFIABLE }, 24, COLOR_16), ImageFileProblem::FLAGS,
        "The header's flags 0x0020 are not allowed in an image file."
    );

    ExpectFailure(
        ParseWhole({ .flags = LV_IMAGE_FLAGS_CUSTOM_DRAW }, 24, COLOR_16), ImageFileProblem::FLAGS,
        "The header's flags 0x0040 are not allowed in an image file."
    );

    ExpectFailure(
        ParseWhole({ .flags = LV_IMAGE_FLAGS_USER1 }, 24, COLOR_16), ImageFileProblem::FLAGS,
        "The header's flags 0x0100 are not allowed in an image file."
    );

    ExpectFailure(
        ParseWhole({ .flags = LV_IMAGE_FLAGS_USER8 }, 24, COLOR_16), ImageFileProblem::FLAGS,
        "The header's flags 0x8000 are not allowed in an image file."
    );
}

ZTEST(image_file, test_a_flag_next_to_compressed_is_refused_where_the_build_decompresses) {
    const std::vector<uint8_t> head = CompressedHead({ .flags = 0 }, LZ4_METHOD, 10, 24);
    auto with_premultiplied = head;
    with_premultiplied[2] = static_cast<uint8_t>(COMPRESSED | LV_IMAGE_FLAGS_PREMULTIPLIED);

    ExpectFailure(
        Parse(with_premultiplied, 34, COLOR_16_LZ4), ImageFileProblem::FLAGS,
        "The header's flags 0x0009 are not allowed in an image file."
    );
}

ZTEST(image_file, test_a_compressed_file_is_refused_by_a_build_that_decompresses_nothing) {
    const auto head = CompressedHead({}, LZ4_METHOD, 10, 24);

    ExpectFailure(
        Parse(head, 34, COLOR_16), ImageFileProblem::COMPRESSION,
        "The image is compressed, and this unit decompresses no images."
    );
}

ZTEST(image_file, test_reserved_bytes_must_be_zero) {
    ExpectFailure(
        ParseWhole({ .reserved = 0x0001 }, 24, COLOR_16), ImageFileProblem::RESERVED,
        "The header's reserved bytes are 0x0001, not 0."
    );

    ExpectFailure(
        ParseWhole({ .reserved = 0xBEEF }, 24, COLOR_16), ImageFileProblem::RESERVED,
        "The header's reserved bytes are 0xBEEF, not 0."
    );
}

ZTEST(image_file, test_an_image_is_at_least_one_pixel_each_way) {
    ExpectFailure(
        ParseWhole({ .w = 0, .stride = 0 }, 0, COLOR_16), ImageFileProblem::ZERO_SIZE,
        "An image is at least 1 by 1 pixels; this one is 0 by 3."
    );

    ExpectFailure(
        ParseWhole({ .h = 0 }, 0, COLOR_16), ImageFileProblem::ZERO_SIZE,
        "An image is at least 1 by 1 pixels; this one is 4 by 0."
    );

    ExpectFailure(
        ParseWhole({ .w = 0, .h = 0, .stride = 0 }, 0, COLOR_16), ImageFileProblem::ZERO_SIZE,
        "An image is at least 1 by 1 pixels; this one is 0 by 0."
    );
}

ZTEST(image_file, test_the_stride_is_at_least_the_least_stride_of_the_format) {
    ExpectFailure(
        ParseWhole({ .cf = RGB565, .w = 4, .stride = 7 }, 21, COLOR_16), ImageFileProblem::STRIDE,
        "A row of 4 pixels in colour format 0x12 takes at least 8 bytes; the stride is 7."
    );

    ExpectFailure(
        ParseWhole({ .cf = RGB565A8, .w = 5, .stride = 8 }, 36, COLOR_16), ImageFileProblem::STRIDE,
        "A row of 5 pixels in colour format 0x14 takes at least 10 bytes; the stride is 8."
    );

    ExpectFailure(
        ParseWhole({ .cf = L8, .w = 5, .stride = 4 }, 12, COLOR_8), ImageFileProblem::STRIDE,
        "A row of 5 pixels in colour format 0x06 takes at least 5 bytes; the stride is 4."
    );

    ExpectFailure(
        ParseWhole({ .cf = AL88, .w = 5, .stride = 9 }, 27, COLOR_8), ImageFileProblem::STRIDE,
        "A row of 5 pixels in colour format 0x15 takes at least 10 bytes; the stride is 9."
    );
}

ZTEST(image_file, test_the_stride_of_an_rgb565a8_image_must_be_even) {
    ExpectFailure(
        ParseWhole({ .cf = RGB565A8, .w = 4, .stride = 9 }, 40, COLOR_16), ImageFileProblem::STRIDE,
        "The stride of an RGB565A8 image must be even; this one is 9."
    );
}

ZTEST(image_file, test_the_pixels_may_take_no_more_than_the_cap) {
    // 1000 x 1000 RGB565 is 2,000,000 bytes: under a 1,999,999 cap by one byte.
    const ImageFileRules capped{ .formats = COLOR_16_FORMATS, .compressions = {}, .max_pixel_bytes = 1999999 };

    ExpectFailure(
        ParseWhole({ .cf = RGB565, .w = 1000, .h = 1000, .stride = 2000 }, 2000000, capped),
        ImageFileProblem::TOO_LARGE, "The pixels take 2000000 bytes, more than the 1999999 this unit loads."
    );

    const ImageFileRules exact{ .formats = COLOR_16_FORMATS, .compressions = {}, .max_pixel_bytes = 2000000 };
    zassert_true(ParseWhole({ .cf = RGB565, .w = 1000, .h = 1000, .stride = 2000 }, 2000000, exact).has_value());
}

ZTEST(image_file, test_the_alpha_plane_counts_toward_the_cap) {
    // 100 x 100 RGB565A8 is 20,000 + 10,000 bytes.
    const ImageFileRules capped{ .formats = COLOR_16_FORMATS, .compressions = {}, .max_pixel_bytes = 29999 };

    ExpectFailure(
        ParseWhole({ .cf = RGB565A8, .w = 100, .h = 100, .stride = 200 }, 30000, capped), ImageFileProblem::TOO_LARGE,
        "The pixels take 30000 bytes, more than the 29999 this unit loads."
    );
}

ZTEST(image_file, test_a_huge_image_is_too_large_without_overflowing) {
    // 32767 x 65535 RGB565A8 at its least stride takes more than 32 bits.
    const auto wide = ParseWhole({ .cf = RGB565A8, .w = 32767, .h = 65535, .stride = 65534 }, 0, COLOR_16);
    zassert_false(wide.has_value());
    zassert_equal(static_cast<int>(wide.error().problem), static_cast<int>(ImageFileProblem::TOO_LARGE));

    const auto square = ParseWhole({ .cf = L8, .w = 65535, .h = 65535, .stride = 65535 }, 0, COLOR_8);
    zassert_false(square.has_value());
    zassert_equal(static_cast<int>(square.error().problem), static_cast<int>(ImageFileProblem::TOO_LARGE));
}

ZTEST(image_file, test_an_uncompressed_file_is_exactly_the_header_and_the_pixels) {
    // 4 x 3 RGB565: 24 bytes of pixels.
    ExpectFailure(
        ParseWhole({}, 25, COLOR_16), ImageFileProblem::LENGTH,
        "A 4 by 3 image in colour format 0x12 with stride 8 is 36 bytes long; this file is 37."
    );

    ExpectFailure(
        ParseWhole({}, 23, COLOR_16), ImageFileProblem::LENGTH,
        "A 4 by 3 image in colour format 0x12 with stride 8 is 36 bytes long; this file is 35."
    );

    ExpectFailure(
        Parse(Header({}), 12, COLOR_16), ImageFileProblem::LENGTH,
        "A 4 by 3 image in colour format 0x12 with stride 8 is 36 bytes long; this file is 12."
    );

    zassert_true(ParseWhole({}, 24, COLOR_16).has_value());
}

ZTEST(image_file, test_the_length_of_an_rgb565a8_file_includes_its_alpha_plane) {
    // 4 x 3 RGB565A8: 24 bytes of colour and 12 of alpha.
    const HeaderFields fields{ .cf = RGB565A8 };

    ExpectFailure(
        ParseWhole(fields, 24, COLOR_16), ImageFileProblem::LENGTH,
        "A 4 by 3 image in colour format 0x14 with stride 8 is 48 bytes long; this file is 36."
    );

    zassert_true(ParseWhole(fields, 36, COLOR_16).has_value());
}

ZTEST(image_file, test_the_first_rule_a_file_breaks_is_the_one_named) {
    // Not an image beats every later rule.
    ExpectFailure(
        ParseWhole({ .magic = 0x00, .cf = L8, .flags = 1, .w = 0, .reserved = 3 }, 0, COLOR_16),
        ImageFileProblem::NOT_AN_IMAGE, "Not an LVGL image file: it starts with 0x00, not 0x19."
    );

    // The format before the flags.
    ExpectFailure(
        ParseWhole({ .cf = L8, .flags = 1, .w = 0, .reserved = 3 }, 0, COLOR_16), ImageFileProblem::FORMAT,
        "Colour format 0x06 is not one this unit draws (it draws 0x12 RGB565, 0x14 RGB565A8)."
    );

    // The flags before the reserved bytes.
    ExpectFailure(
        ParseWhole({ .flags = 1, .w = 0, .reserved = 3 }, 0, COLOR_16), ImageFileProblem::FLAGS,
        "The header's flags 0x0001 are not allowed in an image file."
    );

    // A compressed file in a build without compression is named before the reserved bytes and the size.
    ExpectFailure(
        ParseWhole({ .flags = COMPRESSED, .w = 0, .reserved = 3 }, 0, COLOR_16), ImageFileProblem::COMPRESSION,
        "The image is compressed, and this unit decompresses no images."
    );

    // The reserved bytes before the size.
    ExpectFailure(
        ParseWhole({ .w = 0, .reserved = 3 }, 0, COLOR_16), ImageFileProblem::RESERVED,
        "The header's reserved bytes are 0x0003, not 0."
    );

    // An empty image before the stride.
    ExpectFailure(
        ParseWhole({ .w = 0, .stride = 1 }, 3, COLOR_16), ImageFileProblem::ZERO_SIZE,
        "An image is at least 1 by 1 pixels; this one is 0 by 3."
    );

    // The stride before the cap.
    const ImageFileRules tiny{ .formats = COLOR_16_FORMATS, .compressions = {}, .max_pixel_bytes = 1 };
    ExpectFailure(
        ParseWhole({ .stride = 7 }, 21, tiny), ImageFileProblem::STRIDE,
        "A row of 4 pixels in colour format 0x12 takes at least 8 bytes; the stride is 7."
    );

    // The cap before the length.
    ExpectFailure(
        ParseWhole({}, 1, tiny), ImageFileProblem::TOO_LARGE,
        "The pixels take 24 bytes, more than the 1 this unit loads."
    );
}

// Each format's least stride and pixel bytes.

ZTEST(image_file, test_each_formats_least_stride_and_pixel_bytes) {
    struct Case {
        uint8_t cf;
        const ImageFileRules& rules;
        uint16_t least_stride;
        uint32_t pixel_bytes;
    };

    // 5 x 3 images at their least strides.
    const std::array cases = {
        Case{ L8, COLOR_8, 5, 15 },
        Case{ RGB565, COLOR_16, 10, 30 },
        Case{ RGB565A8, COLOR_16, 10, 45 },
        Case{ AL88, COLOR_8, 10, 30 },
    };

    for(const auto& item : cases) {
        const HeaderFields fields{ .cf = item.cf, .w = 5, .h = 3, .stride = item.least_stride };

        const auto result = ParseWhole(fields, item.pixel_bytes, item.rules);
        zassert_true(result.has_value(), "Format 0x%02X", item.cf);
        zassert_equal(result->pixel_bytes, item.pixel_bytes, "Format 0x%02X", item.cf);
        zassert_equal(static_cast<uint32_t>(result->header.cf), item.cf);
        zassert_equal(static_cast<uint32_t>(result->header.w), 5U);
        zassert_equal(static_cast<uint32_t>(result->header.h), 3U);
        zassert_equal(static_cast<uint32_t>(result->header.stride), item.least_stride);
        zassert_equal(static_cast<uint32_t>(result->header.flags), 0U);
        zassert_equal(static_cast<uint32_t>(result->header.magic), static_cast<uint32_t>(LV_IMAGE_HEADER_MAGIC));
        zassert_equal(static_cast<int>(result->compression), static_cast<int>(LV_IMAGE_COMPRESS_NONE));
        zassert_equal(result->compressed_bytes, 0U);
        zassert_equal(result->data_offset, 12U);

        // One byte less stride is refused.
        const HeaderFields narrower{
            .cf = item.cf, .w = 5, .h = 3, .stride = static_cast<uint16_t>(item.least_stride - 1)
        };
        const auto refused = ParseWhole(narrower, item.pixel_bytes, item.rules);
        zassert_false(refused.has_value(), "Format 0x%02X", item.cf);
        zassert_equal(static_cast<int>(refused.error().problem), static_cast<int>(ImageFileProblem::STRIDE));
    }
}

ZTEST(image_file, test_a_padded_stride_is_taken_and_counts_in_the_pixel_bytes) {
    // A 5 x 3 row padded to 16 bytes.
    const auto l8 = ParseWhole({ .cf = L8, .w = 5, .h = 3, .stride = 16 }, 48, COLOR_8);
    zassert_true(l8.has_value());
    zassert_equal(l8->pixel_bytes, 48U);

    const auto rgb565 = ParseWhole({ .cf = RGB565, .w = 5, .h = 3, .stride = 16 }, 48, COLOR_16);
    zassert_true(rgb565.has_value());
    zassert_equal(rgb565->pixel_bytes, 48U);

    const auto al88 = ParseWhole({ .cf = AL88, .w = 5, .h = 3, .stride = 16 }, 48, COLOR_8);
    zassert_true(al88.has_value());
    zassert_equal(al88->pixel_bytes, 48U);

    // The alpha plane's rows are half the stride: 16 * 3 + 8 * 3.
    const auto rgb565a8 = ParseWhole({ .cf = RGB565A8, .w = 5, .h = 3, .stride = 16 }, 72, COLOR_16);
    zassert_true(rgb565a8.has_value());
    zassert_equal(rgb565a8->pixel_bytes, 72U);
}

// Compressed files, for rules that list LZ4.

ZTEST(image_file, test_a_compressed_head_gives_the_method_the_sizes_and_where_the_data_begins) {
    // A 4 x 3 RGB565 image decompresses to 24 bytes; 10 bytes of LZ4 follow the 24 bytes of heads.
    const auto head = CompressedHead({}, LZ4_METHOD, 10, 24);

    const auto result = Parse(head, 34, COLOR_16_LZ4);
    zassert_true(result.has_value(), "%s", result.has_value() ? "" : result.error().message.c_str());
    zassert_equal(static_cast<int>(result->compression), static_cast<int>(LV_IMAGE_COMPRESS_LZ4));
    zassert_equal(result->compressed_bytes, 10U);
    zassert_equal(result->pixel_bytes, 24U);
    zassert_equal(result->data_offset, 24U);
    zassert_equal(static_cast<uint32_t>(result->header.flags), static_cast<uint32_t>(COMPRESSED));
    zassert_equal(static_cast<uint32_t>(result->header.w), 4U);
    zassert_equal(static_cast<uint32_t>(result->header.h), 3U);
}

ZTEST(image_file, test_a_compressed_rgb565a8_image_decompresses_to_its_colour_and_alpha_planes) {
    const HeaderFields fields{ .cf = RGB565A8 };

    const auto result = Parse(CompressedHead(fields, LZ4_METHOD, 20, 36), 44, COLOR_16_LZ4);
    zassert_true(result.has_value());
    zassert_equal(result->pixel_bytes, 36U);

    ExpectFailure(
        Parse(CompressedHead(fields, LZ4_METHOD, 20, 24), 44, COLOR_16_LZ4), ImageFileProblem::COMPRESSION,
        "The image decompresses to 24 bytes, not the 36 its pixels take."
    );
}

ZTEST(image_file, test_a_compressed_file_needs_24_bytes_of_heads) {
    // The image header says compressed, and the file ends before the compressed-data header does.
    const auto head = Header({ .flags = COMPRESSED });

    ExpectFailure(
        Parse(head, 12, COLOR_16_LZ4), ImageFileProblem::TOO_SHORT,
        "A compressed image file starts with 24 bytes of headers; this one is 12 bytes long."
    );

    auto head_of_23 = head;
    head_of_23.resize(23);
    ExpectFailure(
        Parse(head_of_23, 23, COLOR_16_LZ4), ImageFileProblem::TOO_SHORT,
        "A compressed image file starts with 24 bytes of headers; this one is 23 bytes long."
    );
}

ZTEST(image_file, test_a_compression_method_the_build_does_not_list_is_refused) {
    ExpectFailure(
        Parse(CompressedHead({}, LV_IMAGE_COMPRESS_RLE, 10, 24), 34, COLOR_16_LZ4), ImageFileProblem::COMPRESSION,
        "Compression method 1 is not one this unit decompresses."
    );

    ExpectFailure(
        Parse(CompressedHead({}, 0, 10, 24), 34, COLOR_16_LZ4), ImageFileProblem::COMPRESSION,
        "Compression method 0 is not one this unit decompresses."
    );

    ExpectFailure(
        Parse(CompressedHead({}, 7, 10, 24), 34, COLOR_16_LZ4), ImageFileProblem::COMPRESSION,
        "Compression method 7 is not one this unit decompresses."
    );
}

ZTEST(image_file, test_a_compressed_files_decompressed_size_is_the_pixel_bytes) {
    ExpectFailure(
        Parse(CompressedHead({}, LZ4_METHOD, 10, 25), 34, COLOR_16_LZ4), ImageFileProblem::COMPRESSION,
        "The image decompresses to 25 bytes, not the 24 its pixels take."
    );

    ExpectFailure(
        Parse(CompressedHead({}, LZ4_METHOD, 10, 0), 34, COLOR_16_LZ4), ImageFileProblem::COMPRESSION,
        "The image decompresses to 0 bytes, not the 24 its pixels take."
    );
}

ZTEST(image_file, test_a_compressed_file_is_24_bytes_and_its_compressed_size_long) {
    const auto head = CompressedHead({}, LZ4_METHOD, 10, 24);

    ExpectFailure(
        Parse(head, 35, COLOR_16_LZ4), ImageFileProblem::LENGTH,
        "The compressed data is 10 bytes, so the file is 34 bytes long; this one is 35."
    );

    ExpectFailure(
        Parse(head, 33, COLOR_16_LZ4), ImageFileProblem::LENGTH,
        "The compressed data is 10 bytes, so the file is 34 bytes long; this one is 33."
    );

    ExpectFailure(
        Parse(head, 24, COLOR_16_LZ4), ImageFileProblem::LENGTH,
        "The compressed data is 10 bytes, so the file is 34 bytes long; this one is 24."
    );

    // A compressed size that would wrap 32 bits is a length no file has.
    ExpectFailure(
        Parse(CompressedHead({}, LZ4_METHOD, 0xFFFFFFF0U, 24), 34, COLOR_16_LZ4), ImageFileProblem::LENGTH,
        "The compressed data is 4294967280 bytes, so the file is 4294967295 bytes long; this one is 34."
    );
}

ZTEST(image_file, test_the_compressed_rules_run_after_the_header_rules) {
    // A stride too narrow, and a bad compressed size.
    ExpectFailure(
        Parse(CompressedHead({ .stride = 7 }, LZ4_METHOD, 10, 25), 34, COLOR_16_LZ4), ImageFileProblem::STRIDE,
        "A row of 4 pixels in colour format 0x12 takes at least 8 bytes; the stride is 7."
    );

    // The cap counts the decompressed pixels, and comes before the compressed-data rules.
    const ImageFileRules tiny{ .formats = COLOR_16_FORMATS, .compressions = LZ4_ONLY, .max_pixel_bytes = 23 };
    ExpectFailure(
        Parse(CompressedHead({}, LZ4_METHOD, 10, 25), 34, tiny), ImageFileProblem::TOO_LARGE,
        "The pixels take 24 bytes, more than the 23 this unit loads."
    );

    // The method before the decompressed size, which comes before the length.
    ExpectFailure(
        Parse(CompressedHead({}, LV_IMAGE_COMPRESS_RLE, 10, 25), 99, COLOR_16_LZ4), ImageFileProblem::COMPRESSION,
        "Compression method 1 is not one this unit decompresses."
    );

    ExpectFailure(
        Parse(CompressedHead({}, LZ4_METHOD, 10, 25), 99, COLOR_16_LZ4), ImageFileProblem::COMPRESSION,
        "The image decompresses to 25 bytes, not the 24 its pixels take."
    );
}

ZTEST(image_file, test_a_head_longer_than_the_compressed_headers_is_read_to_24_bytes_only) {
    auto head = CompressedHead({}, LZ4_METHOD, 10, 24);
    head.insert(head.end(), { 0xAA, 0xBB, 0xCC });

    zassert_true(Parse(head, 34, COLOR_16_LZ4).has_value());
}

// Files made by tools/LVGLImage.py.

ZTEST(image_file, test_the_files_lvglimage_writes_are_accepted_as_they_are) {
    for(const auto& fixture : FIXTURES) {
        const bool is_compressed = fixture.compressed_bytes != 0;
        const auto& rules = IsColor16(fixture.cf) ? COLOR_16_LZ4 : COLOR_8_LZ4;

        const auto result = ParseImageFileHead(HeadOf(fixture.file), fixture.file.size(), rules);
        zassert_true(
            result.has_value(), "%s: %s", fixture.name, result.has_value() ? "" : result.error().message.c_str()
        );
        zassert_equal(static_cast<uint32_t>(result->header.cf), fixture.cf, "%s", fixture.name);
        zassert_equal(static_cast<uint32_t>(result->header.w), 8U, "%s", fixture.name);
        zassert_equal(static_cast<uint32_t>(result->header.h), 6U, "%s", fixture.name);
        zassert_equal(static_cast<uint32_t>(result->header.stride), fixture.stride, "%s", fixture.name);
        zassert_equal(result->pixel_bytes, fixture.pixel_bytes, "%s", fixture.name);
        zassert_equal(result->compressed_bytes, fixture.compressed_bytes, "%s", fixture.name);
        zassert_equal(result->data_offset, is_compressed ? 24U : 12U, "%s", fixture.name);
        zassert_equal(
            static_cast<int>(result->compression),
            static_cast<int>(is_compressed ? LV_IMAGE_COMPRESS_LZ4 : LV_IMAGE_COMPRESS_NONE), "%s", fixture.name
        );

        // The parser needs nothing more of the file than its first 24 bytes.
        if(!is_compressed)
            zassert_true(
                ParseImageFileHead(fixture.file.first(12), fixture.file.size(), rules).has_value(), "%s", fixture.name
            );
    }
}

ZTEST(image_file, test_the_uncompressed_files_are_accepted_by_the_rules_of_each_depth_that_lists_their_format) {
    for(const auto& fixture : FIXTURES) {
        if(fixture.compressed_bytes != 0)
            continue;

        const auto& drawn = IsColor16(fixture.cf) ? COLOR_16 : COLOR_8;
        const auto& not_drawn = IsColor16(fixture.cf) ? COLOR_8 : COLOR_16;

        zassert_true(
            ParseImageFileHead(HeadOf(fixture.file), fixture.file.size(), drawn).has_value(), "%s", fixture.name
        );

        const auto refused = ParseImageFileHead(HeadOf(fixture.file), fixture.file.size(), not_drawn);
        zassert_false(refused.has_value(), "%s", fixture.name);
        zassert_equal(
            static_cast<int>(refused.error().problem), static_cast<int>(ImageFileProblem::FORMAT), "%s", fixture.name
        );
    }
}

ZTEST(image_file, test_the_compressed_files_are_refused_where_nothing_decompresses) {
    zassert_equal(OPAQUE_RGB565_LZ4[1], RGB565);

    ExpectFailure(
        ParseImageFileHead(HeadOf(OPAQUE_RGB565_LZ4), OPAQUE_RGB565_LZ4.size(), COLOR_16),
        ImageFileProblem::COMPRESSION, "The image is compressed, and this unit decompresses no images."
    );
}

ZTEST(image_file, test_a_fixture_cut_short_or_with_a_byte_more_is_refused) {
    for(const auto& fixture : FIXTURES) {
        const auto& rules = IsColor16(fixture.cf) ? COLOR_16_LZ4 : COLOR_8_LZ4;

        const auto longer = ParseImageFileHead(HeadOf(fixture.file), fixture.file.size() + 1, rules);
        zassert_false(longer.has_value(), "%s", fixture.name);
        zassert_equal(
            static_cast<int>(longer.error().problem), static_cast<int>(ImageFileProblem::LENGTH), "%s", fixture.name
        );

        const auto shorter = ParseImageFileHead(HeadOf(fixture.file), fixture.file.size() - 1, rules);
        zassert_false(shorter.has_value(), "%s", fixture.name);
        zassert_equal(
            static_cast<int>(shorter.error().problem), static_cast<int>(ImageFileProblem::LENGTH), "%s", fixture.name
        );
    }
}

ZTEST(image_file, test_a_fixtures_exact_message_names_its_numbers) {
    // opaque_rgb565 is an 8 x 6 image with stride 16: 96 pixel bytes in a 108-byte file.
    ExpectFailure(
        ParseImageFileHead(HeadOf(OPAQUE_RGB565), OPAQUE_RGB565.size() + 1, COLOR_16), ImageFileProblem::LENGTH,
        "A 8 by 6 image in colour format 0x12 with stride 16 is 108 bytes long; this file is 109."
    );

    // opaque_rgb565_lz4 holds 98 bytes of data after its 24 bytes of heads.
    ExpectFailure(
        ParseImageFileHead(HeadOf(OPAQUE_RGB565_LZ4), OPAQUE_RGB565_LZ4.size() + 1, COLOR_16_LZ4),
        ImageFileProblem::LENGTH, "The compressed data is 98 bytes, so the file is 122 bytes long; this one is 123."
    );
}

// Names.

ZTEST(image_file, test_a_name_is_an_image_file_when_it_ends_in_lvimg) {
    zassert_true(IsImageFileName("needle_15x220.lvimg"));
    zassert_true(IsImageFileName("a.lvimg"));
    zassert_true(IsImageFileName(".lvimg"));
}

ZTEST(image_file, test_the_extension_is_compared_ignoring_ascii_case) {
    zassert_true(IsImageFileName("needle.LVIMG"));
    zassert_true(IsImageFileName("needle.LvImg"));
}

ZTEST(image_file, test_other_names_are_not_image_files) {
    zassert_false(IsImageFileName(""));
    zassert_false(IsImageFileName("lvimg"));
    zassert_false(IsImageFileName("needle.bin"));
    zassert_false(IsImageFileName("needle.svg"));
    zassert_false(IsImageFileName("needle.lvimg.bin"));
    zassert_false(IsImageFileName("needle.lvimg "));
    zassert_false(IsImageFileName("needle.lvimgs"));
    zassert_false(IsImageFileName("needle_lvimg"));
}

// What LIST says.

ZTEST(image_file, test_the_facts_list_gives_are_lvgls_numbers) {
    const auto plain = ParseImageFileHead(HeadOf(OPAQUE_RGB565A8), OPAQUE_RGB565A8.size(), COLOR_16);
    zassert_true(plain.has_value());

    const auto info = ToAssetImageInfo(*plain);
    zassert_equal(info.width, 8U);
    zassert_equal(info.height, 6U);
    zassert_equal(info.format, static_cast<uint8_t>(LV_COLOR_FORMAT_RGB565A8));
    zassert_equal(info.compression, 0U);

    const auto compressed = ParseImageFileHead(HeadOf(OPAQUE_RGB565_LZ4), OPAQUE_RGB565_LZ4.size(), COLOR_16_LZ4);
    zassert_true(compressed.has_value());

    const auto compressed_info = ToAssetImageInfo(*compressed);
    zassert_equal(compressed_info.width, 8U);
    zassert_equal(compressed_info.height, 6U);
    zassert_equal(compressed_info.format, static_cast<uint8_t>(LV_COLOR_FORMAT_RGB565));
    zassert_equal(compressed_info.compression, static_cast<uint8_t>(LV_IMAGE_COMPRESS_LZ4));
}

ZTEST(image_file, test_the_widest_head_gives_16_bit_sizes) {
    const auto result = ParseWhole({ .cf = L8, .w = 65535, .h = 1, .stride = 65535 }, 65535, COLOR_8);
    zassert_true(result.has_value());

    const auto info = ToAssetImageInfo(*result);
    zassert_equal(info.width, 65535U);
    zassert_equal(info.height, 1U);
}
