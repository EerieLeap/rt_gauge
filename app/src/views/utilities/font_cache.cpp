#include "views/utilities/font_cache.h"

#include <algorithm>
#include <new>
#include <utility>

#include <zephyr/logging/log.h>

#include "domain/ui_domain/lvgl_lock.h"
#include "domain/ui_domain/utilities/widget_property_validator.h"
#include "eerie_memory.hpp"
#include "subsys/assets/asset_kind.h"
#include "subsys/assets/asset_provider.h"
#include "subsys/assets/asset_store.h"
#include "utilities/memory/memory_resource_manager.h"

namespace eerie_leap::views::utilities {

using eerie_leap::domain::ui_domain::ScopedLvglLock;
using eerie_leap::domain::ui_domain::utilities::WidgetPropertyValidator;
using eerie_leap::subsys::assets::AssetKind;
using eerie_leap::subsys::assets::AssetsManager;
using eerie_leap::utilities::memory::Mrm;
using eerie_memory::make_shared_pmr;

LOG_MODULE_REGISTER(font_cache_logger);

namespace {

using FaceData = std::shared_ptr<const std::pmr::vector<uint8_t>>;

constexpr int32_t MIN_GLYPH_CACHE_COUNT = 16;
static_assert(CONFIG_LV_TINY_TTF_CACHE_GLYPH_CNT >= MIN_GLYPH_CACHE_COUNT);

// size^2 / 2 is a generous A8 glyph box. Below the glyphs a readout shows at once, the LRU would re-rasterize every
// frame, hence the floor.
uint32_t GlyphCacheCount(int32_t size_px) {
    const int32_t glyph_bytes = size_px * size_px / 2;
    return std::clamp<int32_t>(
        CONFIG_EERIE_LEAP_UI_FONT_GLYPH_CACHE_SIZE / glyph_bytes, MIN_GLYPH_CACHE_COUNT,
        CONFIG_LV_TINY_TTF_CACHE_GLYPH_CNT
    );
}

} // namespace

FontCache::FontCache(std::shared_ptr<AssetsManager> assets_manager) :
    assets_manager_(std::move(assets_manager)),
    faces_(Mrm::GetExtPmr()),
    fonts_(Mrm::GetExtPmr()) {}

std::optional<LvglFont> FontCache::Acquire(std::string_view name, int32_t size_px) {
    if(name.empty())
        return std::nullopt;

    if(size_px < WidgetPropertyValidator::MIN_FONT_SIZE_PX || size_px > WidgetPropertyValidator::MAX_FONT_SIZE_PX) {
        LOG_ERR(
            "Font size %d px is outside %d..%d px.", size_px, WidgetPropertyValidator::MIN_FONT_SIZE_PX,
            WidgetPropertyValidator::MAX_FONT_SIZE_PX
        );
        return std::nullopt;
    }

    const auto provider = assets_manager_ != nullptr ? assets_manager_->Find(AssetKind::FONTS) : nullptr;
    if(provider == nullptr || provider->store == nullptr) {
        LOG_ERR("The unit serves no fonts, so font '%.*s' cannot load.", static_cast<int>(name.size()), name.data());
        return std::nullopt;
    }
    const uint64_t generation = provider->store->GetGeneration();

    try {
        std::erase_if(faces_, [](const Face& face) {
            return face.data.expired();
        });
        std::erase_if(fonts_, [](const Font& font) {
            return font.font.expired();
        });

        for(const auto& entry : fonts_) {
            if(entry.size_px == size_px && entry.generation == generation && entry.name == name) {
                if(auto font = entry.font.lock())
                    return LvglFont{ std::move(font) };
            }
        }

        FaceData data;
        for(const auto& face : faces_) {
            if(face.generation == generation && face.name == name) {
                data = face.data.lock();
                break;
            }
        }

        if(data == nullptr) {
            auto bytes = provider->store->Load(name);
            if(bytes.empty()) {
                LOG_ERR("Font '%.*s' is missing or empty.", static_cast<int>(name.size()), name.data());
                return std::nullopt;
            }

            data = make_shared_pmr<std::pmr::vector<uint8_t>>(Mrm::GetExtPmr(), std::move(bytes));
            faces_.push_back(Face{ std::pmr::string(name, Mrm::GetExtPmr()), generation, data });
        }

        auto* tiny_ttf = lv_tiny_ttf_create_data_ex(
            data->data(), data->size(), size_px, LV_FONT_KERNING_NORMAL, GlyphCacheCount(size_px)
        );
        if(tiny_ttf == nullptr) {
            LOG_ERR(
                "Font '%.*s' is no TrueType font, or LVGL is out of memory.", static_cast<int>(name.size()), name.data()
            );
            return std::nullopt;
        }

        // The deleter holds the bytes Tiny TTF points into and nothing of the cache: fonts may outlive it.
        std::shared_ptr<const lv_font_t> font(
            tiny_ttf,
            [data](const lv_font_t* released) {
                const ScopedLvglLock lock;
                lv_tiny_ttf_destroy(const_cast<lv_font_t*>(released));
            },
            std::pmr::polymorphic_allocator<>(Mrm::GetExtPmr())
        );
        fonts_.push_back(Font{ std::pmr::string(name, Mrm::GetExtPmr()), size_px, generation, font });

        return LvglFont{ std::move(font) };
    } catch(const std::bad_alloc&) {
        LOG_ERR("Out of memory loading font '%.*s' at %d px.", static_cast<int>(name.size()), name.data(), size_px);
        return std::nullopt;
    }
}

} // namespace eerie_leap::views::utilities
