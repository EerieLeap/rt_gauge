#include "views/utilities/lvgl_font.h"

#include <memory_resource>
#include <utility>

#include "utilities/memory/memory_resource_manager.h"

namespace eerie_leap::views::utilities {

using eerie_leap::utilities::memory::Mrm;

namespace {

using FontRef = std::shared_ptr<const lv_font_t>;

void ReleaseFontRef(lv_event_t* event) {
    auto* copy = static_cast<FontRef*>(lv_event_get_user_data(event));
    std::pmr::polymorphic_allocator<>(Mrm::GetExtPmr()).delete_object(copy);
}

} // namespace

// The empty owner makes the pointer an alias: no control block, so no allocation and a use_count() of 0.
LvglFont::LvglFont(const lv_font_t* font) : lv_font_(std::shared_ptr<void>{}, font) {}

LvglFont::LvglFont(std::shared_ptr<const lv_font_t> font) : lv_font_(std::move(font)) {}

const lv_font_t* LvglFont::ToLvFont() const {
    return lv_font_.get();
}

void LvglFont::BindTo(lv_obj_t* object) const {
    // A compiled font has no owner (use_count() == 0): it is static and needs no reference.
    if(lv_font_.use_count() == 0)
        return;

    std::pmr::polymorphic_allocator<> allocator(Mrm::GetExtPmr());
    auto* copy = allocator.new_object<FontRef>(lv_font_);
    if(lv_obj_add_event_cb(object, ReleaseFontRef, LV_EVENT_DELETE, copy) == nullptr)
        allocator.delete_object(copy);
}

} // namespace eerie_leap::views::utilities
