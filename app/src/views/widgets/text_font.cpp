#include <memory_resource>
#include <string>
#include <string_view>

#include "domain/ui_domain/models/widget_property.h"

#include "text_font.h"

namespace eerie_leap::views::widgets {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;

void TextFont::RegisterProperties(WidgetPropertyStore& store, int32_t default_size_px) {
    store.Register(
        WidgetPropertyType::FONT_FILE_PATH, ConfigValue{ std::pmr::string{} }, PropertyChangeEffect::Rebuild
    );
    store.Register(WidgetPropertyType::FONT_SIZE_PX, ConfigValue{ default_size_px }, PropertyChangeEffect::Rebuild);
}

std::optional<LvglFont>
TextFont::Acquire(const WidgetPropertyStore& properties, const std::shared_ptr<FontCache>& cache) {
    if(cache == nullptr)
        return std::nullopt;

    const auto path = properties.GetAs<std::pmr::string>(WidgetPropertyType::FONT_FILE_PATH, "");
    if(path.empty())
        return std::nullopt;

    return cache->Acquire(std::string_view{ path }, properties.GetAs<int>(WidgetPropertyType::FONT_SIZE_PX, 0));
}

} // namespace eerie_leap::views::widgets
