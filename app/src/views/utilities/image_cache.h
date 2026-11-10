#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <string>
#include <string_view>
#include <vector>

#include <lvgl.h>

#include "subsys/assets/assets_manager.h"

namespace eerie_leap::views::utilities {

/**
 * @brief An image file's pixels in RAM and the descriptor LVGL draws them from, shared by every icon that shows the
 *        file.
 *
 * The pixels sit on the external heap at LV_DRAW_BUF_ALIGN, so the draw units read rows from where they expect them.
 * The destructor takes the LVGL lock and drops the descriptor's entry from LVGL's image cache, which keys a variable
 * source by the descriptor's address: an icon sets its image object's source to null before it lets go of its
 * LoadedImage, as the object would otherwise read what is freed.
 */
class LoadedImage {
public:
    /// Frees a block of the external heap with the size and alignment it was allocated with.
    struct PixelDeleter {
        std::pmr::memory_resource* resource = nullptr;
        size_t bytes = 0;
        size_t alignment = 0;

        void operator()(uint8_t* pixels) const noexcept;
    };

    using Pixels = std::unique_ptr<uint8_t[], PixelDeleter>;

    /// @p header is the file's, as it reads; the pixels hold exactly @p byte_count bytes laid out as it says.
    LoadedImage(const lv_image_header_t& header, Pixels pixels, size_t byte_count);
    ~LoadedImage();

    LoadedImage(const LoadedImage&) = delete;
    LoadedImage& operator=(const LoadedImage&) = delete;
    LoadedImage(LoadedImage&&) = delete;
    LoadedImage& operator=(LoadedImage&&) = delete;

    [[nodiscard]] const lv_image_dsc_t* GetDescriptor() const {
        return &descriptor_;
    }

    [[nodiscard]] size_t GetByteCount() const {
        return descriptor_.data_size;
    }

private:
    lv_image_dsc_t descriptor_{};
    Pixels pixels_;
};

/**
 * @brief The self-describing image files (`.lvimg`) clients upload as the `images` asset kind, loaded once and shared.
 *
 * Widgets that show one file share one LoadedImage. Entries are weak: an image lives as long as an icon holds it. An
 * upload or delete in the kind changes the store's generation, so the next Acquire() loads the new file while the
 * icons already holding the old one keep it until they are built again.
 *
 * Not thread-safe: call it with the LVGL lock held, as every render path does.
 */
class ImageCache {
private:
    struct Entry {
        std::pmr::string name;
        uint64_t generation;
        std::weak_ptr<const LoadedImage> image;
    };

    std::shared_ptr<eerie_leap::subsys::assets::AssetsManager> assets_manager_;
    std::pmr::vector<Entry> entries_;

    std::shared_ptr<const LoadedImage> Load(const eerie_leap::subsys::assets::AssetStore& store, std::string_view name);

public:
    explicit ImageCache(std::shared_ptr<eerie_leap::subsys::assets::AssetsManager> assets_manager);

    /**
     * @return The `.lvimg` file @p name of the `images` kind. nullptr, logged with the parser's message or the
     *         failure, when the unit serves no images, the file is missing, is refused or does not fit the memory;
     *         nullptr without a log for an empty name.
     */
    std::shared_ptr<const LoadedImage> Acquire(std::string_view name);

    /** @return The bytes of the images the cache holds in RAM right now. */
    [[nodiscard]] size_t GetLoadedBytes() const;
};

} // namespace eerie_leap::views::utilities
