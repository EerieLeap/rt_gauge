#include <algorithm>
#include <array>
#include <cstdio>
#include <limits>
#include <span>
#include <string>
#include <utility>

#include "utilities/string/ascii_case.h"

#include "image_file.h"

namespace eerie_leap::domain::ui_domain::images {

using eerie_leap::subsys::assets::AssetImageInfo;
using eerie_leap::utilities::string::EqualsIgnoreAsciiCase;

namespace {

/// The bytes LVGL's image header takes at the start of a file; a compressed file's data header follows it.
constexpr size_t IMAGE_HEADER_SIZE = 12;
/// A message is a sentence with a few numbers; the longest one, the list of formats, stays well under this.
constexpr size_t MESSAGE_BUFFER_SIZE = 256;
/// The low bits of the compressed-data header's method word; LVGL reads no more, whatever the rest holds.
constexpr uint32_t COMPRESSION_METHOD_MASK = 0x0F;
/// Bytes a pixel takes in a plane, least stride of RGB565A8's colour plane for one.
constexpr uint32_t BYTES_PER_PIXEL_16 = 2;

/// What the parser knows of a colour format, held here so that it calls no LVGL function.
struct FormatFacts {
    lv_color_format_t format;
    const char* name;
    /// A row of the colour plane takes at least this many bytes for each pixel.
    uint32_t bytes_per_pixel;
    /// The A8 plane after the colour plane, whose rows are half the stride: RGB565A8.
    bool has_alpha_plane;
};

constexpr std::array FORMATS = {
    FormatFacts{ LV_COLOR_FORMAT_L8, "L8", 1, false },
    FormatFacts{ LV_COLOR_FORMAT_RGB565, "RGB565", BYTES_PER_PIXEL_16, false },
    FormatFacts{ LV_COLOR_FORMAT_RGB565A8, "RGB565A8", BYTES_PER_PIXEL_16, true },
    FormatFacts{ LV_COLOR_FORMAT_AL88, "AL88", BYTES_PER_PIXEL_16, false },
};

const FormatFacts* FindFormat(uint8_t color_format) {
    const auto* const it = std::ranges::find_if(FORMATS, [color_format](const FormatFacts& facts) {
        return static_cast<uint8_t>(facts.format) == color_format;
    });
    return it == FORMATS.end() ? nullptr : &*it;
}

#if LV_COLOR_DEPTH == 16
constexpr std::array BUILD_FORMATS = { LV_COLOR_FORMAT_RGB565, LV_COLOR_FORMAT_RGB565A8 };
#elif LV_COLOR_DEPTH == 8
constexpr std::array BUILD_FORMATS = { LV_COLOR_FORMAT_L8, LV_COLOR_FORMAT_AL88 };
#else
// A build of another depth draws no image file until it names the formats it draws.
constexpr std::span<const lv_color_format_t> BUILD_FORMATS{};
#endif

// Add the methods a build decompresses here, behind the Kconfig symbol that brings the decompressor.
constexpr std::span<const lv_image_compress_t> BUILD_COMPRESSIONS{};

uint16_t ReadU16(std::span<const uint8_t> bytes, size_t at) {
    return static_cast<uint16_t>(bytes[at] | (bytes[at + 1] << 8));
}

uint32_t ReadU32(std::span<const uint8_t> bytes, size_t at) {
    return static_cast<uint32_t>(ReadU16(bytes, at)) | (static_cast<uint32_t>(ReadU16(bytes, at + 2)) << 16);
}

/// @p value for a %u: a number past 32 bits is only ever one a rule refuses for being too large.
uint32_t Clamp32(uint64_t value) {
    return static_cast<uint32_t>(std::min<uint64_t>(value, std::numeric_limits<uint32_t>::max()));
}

std::unexpected<ImageFileError> Fail(ImageFileProblem problem, const char* message) {
    return std::unexpected(ImageFileError{ .problem = problem, .message = message });
}

std::string FormatList(std::span<const lv_color_format_t> formats) {
    std::string list;
    for(const auto format : formats) {
        if(!list.empty())
            list += ", ";

        char item[24];
        const auto* facts = FindFormat(static_cast<uint8_t>(format));
        std::snprintf(
            item, sizeof(item), "0x%02X %s", static_cast<unsigned>(format), facts != nullptr ? facts->name : "?"
        );
        list += item;
    }
    return list;
}

} // namespace

const ImageFileRules& ImageFileRules::OfThisBuild() {
    static constexpr ImageFileRules RULES{
        .formats = BUILD_FORMATS,
        .compressions = BUILD_COMPRESSIONS,
        .max_pixel_bytes = CONFIG_EERIE_LEAP_UI_IMAGE_MAX_SIZE,
    };
    return RULES;
}

std::expected<ImageFileHead, ImageFileError>
ParseImageFileHead(std::span<const uint8_t> head, size_t file_length, const ImageFileRules& rules) {
    char text[MESSAGE_BUFFER_SIZE];

    // A head shorter than its file is the caller's slip; reading no further than it holds keeps the parser safe.
    const size_t known_length = std::min(head.size(), file_length);

    if(known_length < IMAGE_HEADER_SIZE) {
        std::snprintf(
            text, sizeof(text), "An image file starts with a 12-byte header; this one is %zu bytes long.", file_length
        );
        return Fail(ImageFileProblem::TOO_SHORT, text);
    }

    if(head[0] != LV_IMAGE_HEADER_MAGIC) {
        std::snprintf(
            text, sizeof(text), "Not an LVGL image file: it starts with 0x%02X, not 0x19.",
            static_cast<unsigned>(head[0])
        );
        return Fail(ImageFileProblem::NOT_AN_IMAGE, text);
    }

    const uint8_t color_format = head[1];
    const FormatFacts* facts = FindFormat(color_format);
    const bool is_listed = facts != nullptr
        && std::ranges::any_of(rules.formats, [color_format](lv_color_format_t listed) {
               return static_cast<uint8_t>(listed) == color_format;
           });
    if(!is_listed) {
        std::snprintf(
            text, sizeof(text), "Colour format 0x%02X is not one this unit draws (it draws %s).",
            static_cast<unsigned>(color_format), FormatList(rules.formats).c_str()
        );
        return Fail(ImageFileProblem::FORMAT, text);
    }

    // The flags describe buffers, not files: of all of them only COMPRESSED may be in a file, and only where
    // the build decompresses.
    const uint16_t flags = ReadU16(head, 2);
    const bool is_compressed = flags == LV_IMAGE_FLAGS_COMPRESSED;
    if(is_compressed && rules.compressions.empty())
        return Fail(ImageFileProblem::COMPRESSION, "The image is compressed, and this unit decompresses no images.");

    if(flags != 0 && !is_compressed) {
        std::snprintf(
            text, sizeof(text), "The header's flags 0x%04X are not allowed in an image file.",
            static_cast<unsigned>(flags)
        );
        return Fail(ImageFileProblem::FLAGS, text);
    }

    const uint16_t reserved = ReadU16(head, 10);
    if(reserved != 0) {
        std::snprintf(
            text, sizeof(text), "The header's reserved bytes are 0x%04X, not 0.", static_cast<unsigned>(reserved)
        );
        return Fail(ImageFileProblem::RESERVED, text);
    }

    const uint16_t width = ReadU16(head, 4);
    const uint16_t height = ReadU16(head, 6);
    if(width < 1 || height < 1) {
        std::snprintf(
            text, sizeof(text), "An image is at least 1 by 1 pixels; this one is %u by %u.",
            static_cast<unsigned>(width), static_cast<unsigned>(height)
        );
        return Fail(ImageFileProblem::ZERO_SIZE, text);
    }

    const uint16_t stride = ReadU16(head, 8);
    const uint32_t least_stride = width * facts->bytes_per_pixel;
    if(stride < least_stride) {
        std::snprintf(
            text, sizeof(text), "A row of %u pixels in colour format 0x%02X takes at least %u bytes; the stride is %u.",
            static_cast<unsigned>(width), static_cast<unsigned>(color_format), static_cast<unsigned>(least_stride),
            static_cast<unsigned>(stride)
        );
        return Fail(ImageFileProblem::STRIDE, text);
    }

    // The alpha plane's rows are half the colour plane's, as LVGL draws them.
    if(facts->has_alpha_plane && (stride % 2) != 0) {
        std::snprintf(
            text, sizeof(text), "The stride of an RGB565A8 image must be even; this one is %u.",
            static_cast<unsigned>(stride)
        );
        return Fail(ImageFileProblem::STRIDE, text);
    }

    uint64_t pixel_bytes = static_cast<uint64_t>(stride) * height;
    if(facts->has_alpha_plane)
        pixel_bytes += static_cast<uint64_t>(stride / 2) * height;

    if(pixel_bytes > rules.max_pixel_bytes) {
        std::snprintf(
            text, sizeof(text), "The pixels take %u bytes, more than the %u this unit loads.",
            static_cast<unsigned>(Clamp32(pixel_bytes)), static_cast<unsigned>(rules.max_pixel_bytes)
        );
        return Fail(ImageFileProblem::TOO_LARGE, text);
    }

    ImageFileHead result;
    result.header.magic = LV_IMAGE_HEADER_MAGIC;
    result.header.cf = color_format;
    result.header.flags = flags;
    result.header.w = width;
    result.header.h = height;
    result.header.stride = stride;
    result.header.reserved_2 = 0;
    result.pixel_bytes = static_cast<uint32_t>(pixel_bytes);

    if(!is_compressed) {
        const uint64_t expected_length = IMAGE_HEADER_SIZE + pixel_bytes;
        if(file_length != expected_length) {
            std::snprintf(
                text, sizeof(text),
                "A %u by %u image in colour format 0x%02X with stride %u is %u bytes long; this file is %zu.",
                static_cast<unsigned>(width), static_cast<unsigned>(height), static_cast<unsigned>(color_format),
                static_cast<unsigned>(stride), static_cast<unsigned>(Clamp32(expected_length)), file_length
            );
            return Fail(ImageFileProblem::LENGTH, text);
        }

        return result;
    }

    if(known_length < IMAGE_FILE_HEAD_SIZE) {
        std::snprintf(
            text, sizeof(text), "A compressed image file starts with 24 bytes of headers; this one is %zu bytes long.",
            file_length
        );
        return Fail(ImageFileProblem::TOO_SHORT, text);
    }

    const uint32_t method = ReadU32(head, IMAGE_HEADER_SIZE) & COMPRESSION_METHOD_MASK;
    const uint32_t compressed_bytes = ReadU32(head, IMAGE_HEADER_SIZE + 4);
    const uint32_t decompressed_bytes = ReadU32(head, IMAGE_HEADER_SIZE + 8);

    const bool is_method_listed = std::ranges::any_of(rules.compressions, [method](lv_image_compress_t listed) {
        return static_cast<uint32_t>(listed) == method;
    });
    if(!is_method_listed) {
        std::snprintf(
            text, sizeof(text), "Compression method %u is not one this unit decompresses.",
            static_cast<unsigned>(method)
        );
        return Fail(ImageFileProblem::COMPRESSION, text);
    }

    if(decompressed_bytes != result.pixel_bytes) {
        std::snprintf(
            text, sizeof(text), "The image decompresses to %u bytes, not the %u its pixels take.",
            static_cast<unsigned>(decompressed_bytes), static_cast<unsigned>(result.pixel_bytes)
        );
        return Fail(ImageFileProblem::COMPRESSION, text);
    }

    const uint64_t expected_length = IMAGE_FILE_HEAD_SIZE + static_cast<uint64_t>(compressed_bytes);
    if(file_length != expected_length) {
        std::snprintf(
            text, sizeof(text), "The compressed data is %u bytes, so the file is %u bytes long; this one is %zu.",
            static_cast<unsigned>(compressed_bytes), static_cast<unsigned>(Clamp32(expected_length)), file_length
        );
        return Fail(ImageFileProblem::LENGTH, text);
    }

    result.compression = static_cast<lv_image_compress_t>(method);
    result.compressed_bytes = compressed_bytes;
    result.data_offset = IMAGE_FILE_HEAD_SIZE;
    return result;
}

bool IsImageFileName(std::string_view name) {
    return name.size() >= IMAGE_FILE_EXTENSION.size()
        && EqualsIgnoreAsciiCase(name.substr(name.size() - IMAGE_FILE_EXTENSION.size()), IMAGE_FILE_EXTENSION);
}

AssetImageInfo ToAssetImageInfo(const ImageFileHead& head) {
    return AssetImageInfo{
        .width = static_cast<uint16_t>(head.header.w),
        .height = static_cast<uint16_t>(head.header.h),
        .format = static_cast<uint8_t>(head.header.cf),
        .compression = static_cast<uint8_t>(head.compression),
    };
}

} // namespace eerie_leap::domain::ui_domain::images
