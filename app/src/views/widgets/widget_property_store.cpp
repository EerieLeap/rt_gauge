#include <algorithm>
#include <utility>

#include "domain/ui_domain/utilities/widget_property_validator.h"
#include "subsys/threading/scoped_mutex.h"

#include "widget_property_store.h"

namespace eerie_leap::views::widgets {

using eerie_leap::domain::ui_domain::utilities::WidgetPropertyValidator;
using eerie_leap::subsys::threading::ScopedMutex;
using eerie_leap::utilities::type::Color;
using eerie_leap::views::utilities::LvglColor;

WidgetPropertyStore::WidgetPropertyStore() {
    k_mutex_init(&lock_);
}

const WidgetPropertyStore::Entry* WidgetPropertyStore::Find(WidgetPropertyType type) const {
    auto it = std::find_if(entries_.begin(), entries_.end(),
        [type](const Entry& entry) { return entry.type == type; });

    return it == entries_.end() ? nullptr : &(*it);
}

WidgetPropertyStore::Entry* WidgetPropertyStore::Find(WidgetPropertyType type) {
    return const_cast<Entry*>(std::as_const(*this).Find(type));
}

void WidgetPropertyStore::Register(WidgetPropertyType type, ConfigValue default_value, PropertyChangeEffect effect) {
    ScopedMutex guard(lock_);

    auto alternative = static_cast<uint8_t>(default_value.index());

    if(const auto index = WidgetPropertyValidator::GetColorPropertyIndex(type)) {
        const auto* text = std::get_if<std::pmr::string>(&default_value);
        colors_[*index] =
            text == nullptr ? std::nullopt : Color::TryParse(*text).value_or(std::nullopt);
        applied_colors_[*index] = colors_[*index];
    }

    if(auto* entry = Find(type)) {
        entry->effect = effect;
        entry->declared_alternative = alternative;
        entry->value = std::move(default_value);

        return;
    }

    entries_.push_back(Entry {
        .type = type,
        .effect = effect,
        .declared_alternative = alternative,
        .value = std::move(default_value)
    });
}

void WidgetPropertyStore::RegisterColor(WidgetPropertyType type) {
    Register(type, ConfigValue { std::pmr::string {} }, PropertyChangeEffect::Repaint);
}

void WidgetPropertyStore::ApplyColor(WidgetPropertyType type) {
    ScopedMutex guard(lock_);
    const auto index = WidgetPropertyValidator::GetColorPropertyIndex(type);
    if(!index)
        return;

    applied_colors_[*index] = colors_[*index];
}

LvglColor WidgetPropertyStore::ResolveColor(WidgetPropertyType type, LvglColor fallback) const {
    ScopedMutex guard(lock_);
    const auto index = WidgetPropertyValidator::GetColorPropertyIndex(type);
    if(!index)
        return fallback;

    const auto& color = applied_colors_[*index];
    return color.has_value() ? LvglColor(*color) : fallback;
}

bool WidgetPropertyStore::IsRegistered(WidgetPropertyType type) const {
    ScopedMutex guard(lock_);

    return Find(type) != nullptr;
}

PropertyChangeEffect WidgetPropertyStore::GetEffect(WidgetPropertyType type) const {
    ScopedMutex guard(lock_);

    const auto* entry = Find(type);

    return entry == nullptr ? PropertyChangeEffect::None : entry->effect;
}

size_t WidgetPropertyStore::GetDeclaredAlternative(WidgetPropertyType type) const {
    ScopedMutex guard(lock_);

    const auto* entry = Find(type);

    return entry == nullptr ? 0 : entry->declared_alternative;
}

bool WidgetPropertyStore::Set(WidgetPropertyType type, const ConfigValue& value) {
    ScopedMutex guard(lock_);

    auto* entry = Find(type);
    if(entry == nullptr)
        return false;

    if(const auto index = WidgetPropertyValidator::GetColorPropertyIndex(type)) {
        const auto* text = std::get_if<std::pmr::string>(&value);
        if(text == nullptr)
            return false;

        const auto parsed = Color::TryParse(*text);
        if(!parsed.has_value())
            return false;

        entry->value = value;
        colors_[*index] = *parsed;

        return true;
    }

    if(WidgetPropertyValidator::IsAppearanceProperty(type) && !WidgetPropertyValidator::IsValidAppearanceValue(type, value))
        return false;

    if(WidgetPropertyValidator::IsAnimationProperty(type)
        && !WidgetPropertyValidator::IsValidAnimationValue(type, value))
        return false;

    entry->value = value;

    return true;
}

ConfigValue WidgetPropertyStore::Get(WidgetPropertyType type) const {
    ScopedMutex guard(lock_);

    const auto* entry = Find(type);

    return entry == nullptr ? ConfigValue { } : entry->value;
}

std::vector<WidgetPropertyType> WidgetPropertyStore::GetRegisteredTypes() const {
    ScopedMutex guard(lock_);

    std::vector<WidgetPropertyType> types;
    types.reserve(entries_.size());

    for(const auto& entry : entries_)
        types.push_back(entry.type);

    return types;
}

} // namespace eerie_leap::views::widgets
