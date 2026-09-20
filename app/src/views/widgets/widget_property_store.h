#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

#include <zephyr/kernel.h>

#include "domain/ui_domain/models/widget_property.h"
#include "domain/ui_domain/utilities/widget_property_validator.h"
#include "utilities/type/config_value.h"
#include "views/utilities/lvgl_color.h"

namespace eerie_leap::views::widgets {

using eerie_leap::domain::ui_domain::models::WidgetPropertyType;
using eerie_leap::utilities::type::ConfigValue;
using eerie_leap::utilities::type::ConfigValueAs;

// What a widget has to do once a property changed. Ordered by cost, so a batch of changes
// coalesces into the strongest one rather than repainting once per property.
enum class PropertyChangeEffect : uint8_t {
    None = 0,
    Repaint,
    Relayout,
    Rebuild
};

// Runtime properties with their defaults and current values; structural data stays in configuration.
// Subscriptions share its lifetime; WidgetBase guards accepted writes against activity and teardown.
class WidgetPropertyStore {
private:
    struct Entry {
        WidgetPropertyType type;
        PropertyChangeEffect effect;
        uint8_t declared_alternative;
        ConfigValue value;
    };

    // A widget declares roughly ten properties, so a scan beats hashing and allocates once.
    std::vector<Entry> entries_;
    using ColorCache = std::array<std::optional<eerie_leap::utilities::type::Color>,
        eerie_leap::domain::ui_domain::utilities::WidgetPropertyValidator::color_properties.size()>;
    ColorCache colors_;
    ColorCache applied_colors_;
    mutable k_mutex lock_;

    Entry* Find(WidgetPropertyType type);
    const Entry* Find(WidgetPropertyType type) const;

public:
    WidgetPropertyStore();

    WidgetPropertyStore(const WidgetPropertyStore&) = delete;
    WidgetPropertyStore& operator=(const WidgetPropertyStore&) = delete;

    // Re-registering replaces the default; structural properties throw std::invalid_argument.
    void Register(WidgetPropertyType type, ConfigValue default_value, PropertyChangeEffect effect);

    void RegisterColor(WidgetPropertyType type);
    void ApplyColor(WidgetPropertyType type);
    eerie_leap::views::utilities::LvglColor ResolveColor(
        WidgetPropertyType type, eerie_leap::views::utilities::LvglColor fallback) const;

    bool IsRegistered(WidgetPropertyType type) const;
    PropertyChangeEffect GetEffect(WidgetPropertyType type) const;

    // The ConfigValue alternative the default was registered with. An inbound event coerces to
    // it, so a publisher cannot change the type a widget reads back.
    size_t GetDeclaredAlternative(WidgetPropertyType type) const;

    // False for structural/unregistered properties or invalid values; the previous value is retained.
    bool Set(WidgetPropertyType type, const ConfigValue& value);

    ConfigValue Get(WidgetPropertyType type) const;

    template<typename T>
    T GetAs(WidgetPropertyType type, const T& fallback) const {
        return ConfigValueAs<T>(Get(type), fallback);
    }

    // In registration order, base class first. Callers arrange any application dependencies.
    std::vector<WidgetPropertyType> GetRegisteredTypes() const;
};

} // namespace eerie_leap::views::widgets
