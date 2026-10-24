#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <memory_resource>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <lvgl.h>

#include "subsys/assets/assets_manager.h"
#include "views/utilities/lvgl_font.h"

namespace eerie_leap::views::utilities {

/**
 * @brief The fonts clients upload as the `fonts` asset kind, rasterized at a size by LVGL's Tiny TTF.
 *
 * Widgets that ask for one file at one size share one lv_font_t and with it one glyph cache, and the sizes of a file
 * share its bytes. Entries are weak: a font lives as long as a label holds it (LvglFont::BindTo()), a file's bytes as
 * long as one of its sizes. An upload or delete in the kind changes the store's generation, so the next Acquire()
 * loads the new file while the fonts already handed out stay as they are.
 *
 * The kind's built-in fonts (AssetProvider::built_ins) are not loaded: Tiny TTF reads the bytes in flash in place, so
 * a built-in costs no RAM copy and no store read, and an upload or delete does not touch its entries. A built-in wins
 * over a stored file of its name, as the assets group serves it.
 *
 * Not thread-safe: call it with the LVGL lock held, as every render path does.
 */
class FontCache {
private:
    struct Face {
        std::pmr::string name;
        uint64_t generation;
        std::weak_ptr<const std::pmr::vector<uint8_t>> data;
    };

    struct Font {
        std::pmr::string name;
        int32_t size_px;
        uint64_t generation;
        std::weak_ptr<const lv_font_t> font;
    };

    std::shared_ptr<eerie_leap::subsys::assets::AssetsManager> assets_manager_;
    std::pmr::vector<Face> faces_;
    std::pmr::vector<Font> fonts_;

public:
    explicit FontCache(std::shared_ptr<eerie_leap::subsys::assets::AssetsManager> assets_manager);

    /**
     * @return The `fonts` asset @p name at @p size_px (the em size in pixels). nullopt, logged, when the unit serves no
     *         fonts, the size is outside WidgetPropertyValidator's font range, the file is missing or is no TrueType
     *         font, or memory ran out; nullopt without a log for an empty name.
     */
    std::optional<LvglFont> Acquire(std::string_view name, int32_t size_px);

    /** @return The bytes of uploaded font files the cache holds in RAM right now; built-in fonts never count. */
    [[nodiscard]] size_t GetCopiedBytes() const;
};

} // namespace eerie_leap::views::utilities
