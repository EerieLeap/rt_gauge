#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <lvgl.h>

#include "subsys/assets/asset_kind.h"

#include "image_file.h"

#include "image_asset_provider.h"

namespace eerie_leap::domain::ui_domain::images {

using eerie_leap::subsys::assets::AssetAccess;
using eerie_leap::subsys::assets::AssetImageInfo;
using eerie_leap::subsys::assets::AssetKind;
using eerie_leap::subsys::assets::AssetProvider;
using eerie_leap::subsys::assets::AssetStore;
using eerie_leap::subsys::assets::ToMask;

namespace {

/// What a client may upload as an image. A provider keeps only a span of its extensions.
constexpr std::array<std::string_view, 3> EXTENSIONS{ ".bin", ".svg", IMAGE_FILE_EXTENSION };

/// The most formats or compression methods a build lists; the lists are copied to numbers once.
constexpr size_t MAX_LISTED = 8;

/// A list of LVGL numbers in static storage, which is what KINDS reports.
struct Numbers {
    std::array<uint32_t, MAX_LISTED> values{};
    size_t count = 0;

    std::span<const uint32_t> Span() const {
        return std::span<const uint32_t>(values).first(count);
    }
};

template<typename T>
Numbers NumbersOf(std::span<const T> listed) {
    Numbers numbers;
    for(const auto item : listed) {
        if(numbers.count == MAX_LISTED)
            break;

        numbers.values[numbers.count++] = static_cast<uint32_t>(item);
    }

    return numbers;
}

const Numbers& BuildFormats() {
    static const Numbers FORMATS = NumbersOf(ImageFileRules::OfThisBuild().formats);
    return FORMATS;
}

const Numbers& BuildCompressions() {
    static const Numbers COMPRESSIONS = NumbersOf(ImageFileRules::OfThisBuild().compressions);
    return COMPRESSIONS;
}

std::optional<AssetImageInfo> GetInfo(const AssetStore& store, std::string_view name) {
    if(!IsImageFileName(name))
        return std::nullopt;

    std::array<uint8_t, IMAGE_FILE_HEAD_SIZE> head{};
    size_t file_size = 0;
    const auto read = store.ReadHead(name, head, file_size);
    if(read < 0)
        return std::nullopt;

    const auto parsed = ParseImageFileHead(
        std::span<const uint8_t>(head).first(static_cast<size_t>(read)), file_size, ImageFileRules::OfThisBuild()
    );
    if(!parsed.has_value())
        return std::nullopt;

    return ToAssetImageInfo(*parsed);
}

std::optional<std::string> CheckContent(std::string_view name, size_t length, std::span<const uint8_t> head) {
    if(!IsImageFileName(name))
        return std::nullopt;

    const auto parsed = ParseImageFileHead(head, length, ImageFileRules::OfThisBuild());
    if(parsed.has_value())
        return std::nullopt;

    return parsed.error().message;
}

} // namespace

AssetProvider MakeImageAssetProvider(std::shared_ptr<AssetStore> store) {
    // Widgets keep RAM copies of what they load, so a client may overwrite or delete a file a widget
    // shows: nothing is ever in use, and the change appears at the widget's next render.
    constexpr auto ACCESS = static_cast<uint8_t>(
        ToMask(AssetAccess::READ) | ToMask(AssetAccess::WRITE) | ToMask(AssetAccess::DELETE)
    );

    AssetProvider provider{ .kind = AssetKind::IMAGES,
        .name = "images",
        .store = store,
        .extensions = EXTENSIONS,
        .access = ACCESS,
        .max_size = CONFIG_EERIE_LEAP_UI_IMAGE_MAX_SIZE,
        // The raw .bin files carry no header: their pixels are in the firmware's colour format.
        .pixel_format = static_cast<uint32_t>(LV_COLOR_FORMAT_NATIVE_WITH_ALPHA) };

    provider.image.formats = BuildFormats().Span();
    provider.image.compressions = BuildCompressions().Span();
    // The listing asks for the facts of every stored .lvimg: the store outlives the provider's callbacks.
    provider.image.get_info = [store = std::move(store)](std::string_view name) {
        return GetInfo(*store, name);
    };
    provider.content_head_size = IMAGE_FILE_HEAD_SIZE;
    provider.check_content = CheckContent;

    return provider;
}

} // namespace eerie_leap::domain::ui_domain::images
