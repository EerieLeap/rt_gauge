#pragma once

#include <cstdint>
#include <vector>

#include <zephyr/kernel.h>

#include "domain/ui_domain/models/widget_property.h"
#include "utilities/type/config_value.h"

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

// Every property a widget declares support for, with its default and its current value.
// Held by shared_ptr so an event subscription can keep writing values while the widget that
// declared them is being torn down.
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
    mutable k_mutex lock_;

    Entry* Find(WidgetPropertyType type);
    const Entry* Find(WidgetPropertyType type) const;

public:
    WidgetPropertyStore();

    WidgetPropertyStore(const WidgetPropertyStore&) = delete;
    WidgetPropertyStore& operator=(const WidgetPropertyStore&) = delete;

    // Re-registering replaces the default, so a derived class can narrow what its base declared.
    void Register(WidgetPropertyType type, ConfigValue default_value, PropertyChangeEffect effect);

    bool IsRegistered(WidgetPropertyType type) const;
    PropertyChangeEffect GetEffect(WidgetPropertyType type) const;

    // The ConfigValue alternative the default was registered with. An inbound event coerces to
    // it, so a publisher cannot change the type a widget reads back.
    size_t GetDeclaredAlternative(WidgetPropertyType type) const;

    // False when unregistered or when color/opacity validation fails; the previous value is retained.
    bool Set(WidgetPropertyType type, const ConfigValue& value);

    ConfigValue Get(WidgetPropertyType type) const;

    template<typename T>
    T GetAs(WidgetPropertyType type, const T& fallback) const {
        return ConfigValueAs<T>(Get(type), fallback);
    }

    // In registration order, which runs base class first, so a replay applies a base property
    // before the derived property that reads it.
    std::vector<WidgetPropertyType> GetRegisteredTypes() const;
};

} // namespace eerie_leap::views::widgets
