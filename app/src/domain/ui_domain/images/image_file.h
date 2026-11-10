#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>

#include <lvgl.h>

#include "subsys/assets/asset_provider.h"

namespace eerie_leap::domain::ui_domain::images {

/// The extension of an image file that describes itself: the extension is the format, and nothing guesses from content.
inline constexpr std::string_view IMAGE_FILE_EXTENSION = ".lvimg";
/// The bytes a head may need: the image header, then LVGL's compressed-data header.
inline constexpr size_t IMAGE_FILE_HEAD_SIZE = 24;

/// What a head says of its file, as LVGL reads it.
struct ImageFileHead {
    /// As in the file, checked.
    lv_image_header_t header{};
    /// What LVGL reads: stride × h, plus RGB565A8's A8 plane.
    uint32_t pixel_bytes = 0;
    lv_image_compress_t compression = LV_IMAGE_COMPRESS_NONE;
    /// The size of the compressed data that follows the 24 bytes of heads; 0 when the file is not compressed.
    uint32_t compressed_bytes = 0;
    /// Where the pixels, or the compressed data, begin: 12, or 24 when compressed.
    uint32_t data_offset = 12;
};

/**
 * @brief What a build takes of an image file.
 *
 * The rules are data so that a build that draws more formats changes OfThisBuild() alone, and the tests can give
 * the parser both colour depths' lists without a build of each.
 */
struct ImageFileRules {
    /// The colour formats the build draws, in static storage.
    std::span<const lv_color_format_t> formats;
    /// The compression methods it decompresses, in static storage; none means a compressed file is refused.
    std::span<const lv_image_compress_t> compressions;
    /// The most pixel bytes a file may have, also once decompressed.
    uint32_t max_pixel_bytes = 0;

    /// This build's: the display's two formats (RGB565 and RGB565A8 with LV_COLOR_DEPTH 16, L8 and AL88 with 8) and
    /// the images kind's cap, CONFIG_EERIE_LEAP_UI_IMAGE_MAX_SIZE. No build decompresses yet.
    static const ImageFileRules& OfThisBuild();
};

/// The rule of an image file that a file breaks, in the order the rules run.
enum class ImageFileProblem : uint8_t {
    TOO_SHORT,
    NOT_AN_IMAGE,
    FORMAT,
    FLAGS,
    RESERVED,
    /// A width or height of 0. Not EMPTY: Zephyr defines that name as a macro.
    ZERO_SIZE,
    STRIDE,
    TOO_LARGE,
    COMPRESSION,
    LENGTH,
};

struct ImageFileError {
    ImageFileProblem problem;
    /// A sentence for the client with its numbers filled in.
    std::string message;
};

/**
 * @brief The head of a file @p file_length bytes long, checked against @p rules.
 *
 * @param head The file's first IMAGE_FILE_HEAD_SIZE bytes, or all of a shorter file.
 *
 * It takes the facts from the head and the file's length alone, so a caller reads no more than the head. A failure
 * names the first rule broken; the message is built only then, and a std::string may throw std::bad_alloc.
 *
 * Pure, with no I/O and no LVGL call (LVGL's header type and constants only), so it needs no LVGL lock.
 */
std::expected<ImageFileHead, ImageFileError>
ParseImageFileHead(std::span<const uint8_t> head, size_t file_length, const ImageFileRules& rules);

/// Whether @p name ends in IMAGE_FILE_EXTENSION, ASCII case ignored, as the assets group compares names.
bool IsImageFileName(std::string_view name);

/// The facts LIST gives of @p head.
eerie_leap::subsys::assets::AssetImageInfo ToAssetImageInfo(const ImageFileHead& head);

} // namespace eerie_leap::domain::ui_domain::images
