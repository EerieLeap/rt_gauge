#include "views/utilities/image_cache.h"

#include <algorithm>
#include <array>
#include <new>
#include <span>
#include <utility>

#include <zephyr/logging/log.h>

#include "domain/ui_domain/images/image_file.h"
#include "domain/ui_domain/lvgl_lock.h"
#include "eerie_memory.hpp"
#include "subsys/assets/asset_kind.h"
#include "subsys/assets/asset_provider.h"
#include "subsys/assets/asset_store.h"
#include "utilities/memory/memory_resource_manager.h"

#include <misc/cache/instance/lv_image_cache.h>

namespace eerie_leap::views::utilities {

using eerie_leap::domain::ui_domain::ScopedLvglLock;
using eerie_leap::domain::ui_domain::images::IMAGE_FILE_HEAD_SIZE;
using eerie_leap::domain::ui_domain::images::ImageFileHead;
using eerie_leap::domain::ui_domain::images::ImageFileRules;
using eerie_leap::domain::ui_domain::images::ParseImageFileHead;
using eerie_leap::subsys::assets::AssetKind;
using eerie_leap::subsys::assets::AssetReader;
using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::subsys::assets::AssetStore;
using eerie_leap::utilities::memory::Mrm;
using eerie_memory::make_shared_pmr;

LOG_MODULE_REGISTER(image_cache_logger);

void LoadedImage::PixelDeleter::operator()(uint8_t* pixels) const noexcept {
    if(pixels != nullptr)
        resource->deallocate(pixels, bytes, alignment);
}

LoadedImage::LoadedImage(const lv_image_header_t& header, Pixels pixels, size_t byte_count) :
    pixels_(std::move(pixels)) {
    descriptor_.header = header;
    // The descriptor describes the pixels in RAM: a flag that says the file was compressed is not true of them.
    descriptor_.header.flags = 0;
    descriptor_.data_size = static_cast<uint32_t>(byte_count);
    descriptor_.data = pixels_.get();
}

LoadedImage::~LoadedImage() {
    // LVGL's image cache keys a variable source by the descriptor's address, which is about to be freed.
    const ScopedLvglLock lock;
    lv_image_cache_drop(&descriptor_);
}

ImageCache::ImageCache(std::shared_ptr<AssetsManager> assets_manager) :
    assets_manager_(std::move(assets_manager)),
    entries_(Mrm::GetExtPmr()) {}

size_t ImageCache::GetLoadedBytes() const {
    size_t total = 0;

    for(const auto& entry : entries_) {
        if(const auto image = entry.image.lock())
            total += image->GetByteCount();
    }

    return total;
}

std::shared_ptr<const LoadedImage> ImageCache::Acquire(std::string_view name) {
    if(name.empty())
        return nullptr;

    const auto provider = assets_manager_ != nullptr ? assets_manager_->Find(AssetKind::IMAGES) : nullptr;
    if(provider == nullptr || provider->store == nullptr) {
        LOG_ERR("The unit serves no images, so image '%.*s' cannot load.", static_cast<int>(name.size()), name.data());
        return nullptr;
    }
    const uint64_t generation = provider->store->GetGeneration();

    try {
        std::erase_if(entries_, [](const Entry& entry) {
            return entry.image.expired();
        });

        for(const auto& entry : entries_) {
            if(entry.generation == generation && entry.name == name) {
                if(auto image = entry.image.lock())
                    return image;
            }
        }

        auto image = Load(*provider->store, name);
        if(image != nullptr)
            entries_.push_back(Entry{ std::pmr::string(name, Mrm::GetExtPmr()), generation, image });

        return image;
    } catch(const std::bad_alloc&) {
        LOG_ERR("Image '%.*s' does not fit the memory.", static_cast<int>(name.size()), name.data());
        return nullptr;
    }
}

std::shared_ptr<const LoadedImage> ImageCache::Load(const AssetStore& store, std::string_view name) {
    AssetReader reader;
    const int open_result = store.OpenRead(name, reader);
    if(open_result < 0) {
        LOG_ERR("Image '%.*s' cannot be opened: %d.", static_cast<int>(name.size()), name.data(), open_result);
        return nullptr;
    }

    std::array<uint8_t, IMAGE_FILE_HEAD_SIZE> head_bytes{};
    const ssize_t head_read = reader.Read(0, head_bytes);
    if(head_read < 0) {
        LOG_ERR(
            "Image '%.*s' cannot be read: %d.", static_cast<int>(name.size()), name.data(), static_cast<int>(head_read)
        );
        return nullptr;
    }

    const auto parsed = ParseImageFileHead(
        std::span<const uint8_t>(head_bytes).first(static_cast<size_t>(head_read)), reader.GetSize(),
        ImageFileRules::OfThisBuild()
    );
    if(!parsed.has_value()) {
        LOG_ERR(
            "Image '%.*s' is refused: %s", static_cast<int>(name.size()), name.data(), parsed.error().message.c_str()
        );
        return nullptr;
    }
    const ImageFileHead& head = parsed.value();

    // Aligned as the draw units read rows, which is also what a swap-copy of 2-byte pixels wants.
    // NOLINTNEXTLINE(misc-const-correctness): the pixels are moved into the image below.
    LoadedImage::Pixels pixels(
        static_cast<uint8_t*>(Mrm::GetExtPmr()->allocate(head.pixel_bytes, LV_DRAW_BUF_ALIGN)),
        LoadedImage::PixelDeleter{ Mrm::GetExtPmr(), head.pixel_bytes, LV_DRAW_BUF_ALIGN }
    );

    // The parser refuses a compressed file until the build lists a method, so every file reaching this point holds
    // its pixels as they are. A method is added here: read head.compressed_bytes from head.data_offset into a
    // temporary buffer on the external heap, decompress that into the pixels, and require exactly head.pixel_bytes
    // out. LVGL must never see a compressed descriptor.
    if(head.compression != LV_IMAGE_COMPRESS_NONE) {
        LOG_ERR(
            "Image '%.*s' is compressed, and this unit decompresses no images.", static_cast<int>(name.size()),
            name.data()
        );
        return nullptr;
    }

    const ssize_t pixels_read = reader.Read(head.data_offset, std::span<uint8_t>(pixels.get(), head.pixel_bytes));
    if(pixels_read < 0 || std::cmp_not_equal(pixels_read, head.pixel_bytes)) {
        LOG_ERR(
            "Image '%.*s' ended after %d of its %u pixel bytes.", static_cast<int>(name.size()), name.data(),
            static_cast<int>(std::max<ssize_t>(pixels_read, 0)), static_cast<unsigned>(head.pixel_bytes)
        );
        return nullptr;
    }

    return make_shared_pmr<LoadedImage>(Mrm::GetExtPmr(), head.header, std::move(pixels), head.pixel_bytes);
}

} // namespace eerie_leap::views::utilities
