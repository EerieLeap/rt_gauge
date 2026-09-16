#include <algorithm>
#include <array>
#include <exception>
#include <utility>

#include <zephyr/logging/log.h>

#include "utilities/reflection/caller_name.h"
#include "utilities/string/string_helpers.h"

#include "domain/ui_domain/models/widget_property.h"

#include "event_bus/event_channel_registry.h"

#include "views/widgets/property_value_conversion.h"

#include "widget_base.h"

namespace eerie_leap::views::widgets {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;

using eerie_leap::event_bus::EventChannelRegistry;
using eerie_leap::utilities::reflection::GetCallerName;
using eerie_leap::utilities::string::StringHelpers;

LOG_MODULE_REGISTER(widget_base_logger);

namespace {

// Configuration names a selector by the readable id it shares with the rest of the system;
// payloads carry it hashed, so the hash is taken once at subscribe time.
std::optional<EventData> ToSelectorValue(const ConfigValue& value) {
    if(const auto* text = std::get_if<std::pmr::string>(&value))
        return EventData { StringHelpers::GetHash(*text) };

    if(const auto* number = std::get_if<int>(&value))
        return EventData { *number };

    if(const auto* flag = std::get_if<bool>(&value))
        return EventData { *flag };

    return std::nullopt;
}

bool SelectorMatches(const EventData& selector, const EventData& candidate) {
    if(const auto* hash = std::get_if<uint32_t>(&selector)) {
        const auto* value = std::get_if<uint32_t>(&candidate);

        return value != nullptr && *value == *hash;
    }

    if(const auto* number = std::get_if<int>(&selector)) {
        if(const auto* value = std::get_if<int>(&candidate))
            return *value == *number;

        const auto* unsigned_value = std::get_if<uint32_t>(&candidate);

        return unsigned_value != nullptr && static_cast<int>(*unsigned_value) == *number;
    }

    const auto* flag = std::get_if<bool>(&selector);
    const auto* value = std::get_if<bool>(&candidate);

    return flag != nullptr && value != nullptr && *value == *flag;
}

} // namespace

WidgetBase::WidgetBase(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context)
    : id_(id), properties_(std::make_shared<WidgetPropertyStore>()), parent_(std::move(parent)),
    dispatch_guard_(std::make_shared<WidgetDispatchGuard>(this)),
    context_(std::move(context)) {

    container_ = std::make_shared<Frame>(Frame::CreateWrapped(parent_->GetObject())
        .SetWidth(100, false)
        .SetHeight(100, false)
        .Build());
}

WidgetBase::~WidgetBase() {
    DetachDispatch();

    // Before any other member goes: each entry unsubscribes as it is destroyed.
    subscriptions_.clear();
}

void WidgetBase::DetachDispatch() {
    dispatch_guard_->Detach();
}

void WidgetBase::AddSubscription(AnySubscription subscription) {
    if(subscription != nullptr)
        subscriptions_.push_back(std::move(subscription));
}

uint32_t WidgetBase::GetId() const {
    return id_;
}

bool WidgetBase::IsActive() const {
    return is_active_;
}

void WidgetBase::OnActivated() {
    is_active_ = true;

    // The store kept tracking while the group was hidden, so this is where the widget catches up.
    if(IsReady())
        ReplayProperties();
}

void WidgetBase::OnDeactivated() {
    is_active_ = false;
}

void WidgetBase::RegisterProperties(WidgetPropertyStore& store) const {
    store.Register(WidgetPropertyType::IS_VISIBLE, ConfigValue { true }, PropertyChangeEffect::None);
    store.Register(WidgetPropertyType::IS_SMOOTHED, ConfigValue { false }, PropertyChangeEffect::None);
}

void WidgetBase::OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) {
    if(type == WidgetPropertyType::IS_VISIBLE)
        SetVisibility(ConfigValueAs<bool>(value, true));
}

void WidgetBase::OnConfigured() { }

void WidgetBase::AddDependency(WidgetBase& dependency) {
    dependencies_.push_back(&dependency);
}

std::vector<WidgetPropertyType> WidgetBase::GetSupportedProperties() const {
    WidgetPropertyStore declared;

    RegisterProperties(declared);

    auto supported = declared.GetRegisteredTypes();

    for(const auto* dependency : dependencies_) {
        for(auto type : dependency->GetSupportedProperties()) {
            if(std::find(supported.begin(), supported.end(), type) == supported.end())
                supported.push_back(type);
        }
    }

    return supported;
}

void WidgetBase::RunEffect(PropertyChangeEffect effect) {
    if(effect == PropertyChangeEffect::None || !IsReady())
        return;

    container_->Invalidate();
}

// Caller holds the LVGL lock and the dispatch guard, in that order.
void WidgetBase::NotifyPropertyChanged(WidgetPropertyType type, const ConfigValue& value, PropertyChangeEffect effect) {
    if(!IsReady() || !IsActive())
        return;

    OnPropertyChanged(type, value);
    RunEffect(effect);
}

void WidgetBase::SetPropertyLocal(WidgetPropertyType type, const ConfigValue& value) {
    static constexpr auto caller = GetCallerName();

    if(!properties_->Set(type, value))
        return;

    for(const auto& binding : outbound_bindings_) {
        if(binding.target != type)
            continue;

        std::array<std::pair<uint32_t, EventData>, 2> payload {
            std::pair { binding.payload_key, ToEventData(value) },
            std::pair { binding.selector_key, binding.selector_value.value_or(EventData { }) }
        };

        binding.channel->PublishErasedAsync(
            binding.event_type,
            caller.hash,
            ErasedPayload { payload.data(), binding.selector_value.has_value() ? 2U : 1U });
    }
}

void WidgetBase::ResolveBindings() {
    auto& registry = EventChannelRegistry::GetInstance();

    for(const auto& binding : configuration_->bindings) {
        if(!properties_->IsRegistered(binding.target)) {
            LOG_WRN("Widget %u binds unsupported property %u.",
                id_, static_cast<unsigned>(binding.target));

            continue;
        }

        auto* channel = registry.Find(binding.channel);
        if(channel == nullptr) {
            LOG_WRN("Widget %u binds property %u to an unregistered channel.",
                id_, static_cast<unsigned>(binding.target));

            continue;
        }

        auto selector = binding.HasSelector() ? ToSelectorValue(binding.selector_value) : std::nullopt;

        if(binding.direction != PropertyBindingDirection::In) {
            outbound_bindings_.push_back(OutboundBinding {
                .target = binding.target,
                .channel = channel,
                .event_type = binding.outbound_event_type,
                .payload_key = binding.payload_key,
                .selector_key = binding.selector_key,
                .selector_value = selector
            });
        }

        if(binding.direction == PropertyBindingDirection::Out)
            continue;

        // The selector goes to the channel, not into the handler: a widget bound to another
        // sensor is then rejected under the subscriber lock instead of waking for every sample.
        ErasedEventFilter filter;
        if(selector.has_value()) {
            filter = [selector_key = binding.selector_key, selector](const ErasedPayloadView& view) {
                const auto* candidate = view.Find(selector_key);

                return candidate != nullptr && SelectorMatches(*selector, *candidate);
            };
        }

        AddSubscription(channel->SubscribeErased(
            binding.event_type,
            std::move(filter),
            [this,
             store = properties_,
             guard = dispatch_guard_,
             target = binding.target,
             effect = properties_->GetEffect(binding.target),
             payload_key = binding.payload_key](const ErasedPayloadView& view) {

                const auto* data = view.Find(payload_key);
                if(data == nullptr)
                    return;

                auto value = CoerceToConfigValue(*data, store->GetDeclaredAlternative(target), target);
                if(std::holds_alternative<std::monostate>(value))
                    return;

                // The store is written through the captured shared_ptr, never through `this`, so
                // the model keeps tracking while the widget is hidden or being destroyed.
                if(!store->Set(target, value))
                    return;

                // Two acquisitions rather than one. Reading the flags under the guard alone is
                // safe; taking the LVGL lock inside the guard would invert the lock order, and
                // taking it unconditionally would put every hidden widget back on the hot path.
                bool is_renderable = false;
                guard->Dispatch([&] { is_renderable = IsReady() && IsActive(); });

                if(!is_renderable)
                    return;

                ScopedLvglLock lvgl_guard;

                guard->Dispatch([&] { NotifyPropertyChanged(target, value, effect); });
            }));
    }
}

void WidgetBase::ReplayProperties() {
    auto strongest = PropertyChangeEffect::None;

    for(auto type : properties_->GetRegisteredTypes()) {
        OnPropertyChanged(type, properties_->Get(type));
        strongest = std::max(strongest, properties_->GetEffect(type));
    }

    RunEffect(strongest);
}

void WidgetBase::Configure(std::shared_ptr<WidgetConfiguration> configuration) {
    ApplyConfiguration(std::move(configuration), true);
}

void WidgetBase::ConfigureAsPart(std::shared_ptr<WidgetConfiguration> configuration) {
    ApplyConfiguration(std::move(configuration), false);
}

void WidgetBase::ApplyConfiguration(std::shared_ptr<WidgetConfiguration> configuration, bool is_owner) {
    configuration_ = std::move(configuration);

    RegisterProperties(*properties_);

    // A dependency reads its own store, so its properties are configurable here but legitimately
    // absent from this one.
    auto supported = is_owner ? GetSupportedProperties() : std::vector<WidgetPropertyType> { };

    for(const auto& [type, value] : configuration_->properties) {
        if(!properties_->Set(type, value) && is_owner
            && std::find(supported.begin(), supported.end(), type) == supported.end())
            LOG_WRN("Widget %u does not support property %u.", id_, static_cast<unsigned>(type));
    }

    ReplayProperties();

    for(auto* dependency : dependencies_)
        dependency->ConfigureAsPart(configuration_);

    OnConfigured();

    if(is_owner)
        ResolveBindings();
}

std::shared_ptr<WidgetConfiguration> WidgetBase::GetConfiguration() const {
    return configuration_;
}

int WidgetBase::SetVisibility(bool is_visible) {
    if(is_visible)
        lv_obj_clear_flag(container_->GetObject(), LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(container_->GetObject(), LV_OBJ_FLAG_HIDDEN);

    return 0;
}

bool WidgetBase::IsVisible() const {
    return properties_->GetAs<bool>(WidgetPropertyType::IS_VISIBLE, true);
}

bool WidgetBase::IsSmoothed() const {
    return properties_->GetAs<bool>(WidgetPropertyType::IS_SMOOTHED, false);
}

WidgetPosition WidgetBase::GetPositionPx() const {
    return position_px_;
}

void WidgetBase::SetPositionPx(const WidgetPosition& position_px) {
    position_px_ = position_px;

    container_->SetXOffset(position_px.x, true)
        .SetYOffset(position_px.y, true);
}

WidgetSize WidgetBase::GetSizePx() const {
    return size_px_;
}

void WidgetBase::SetSizePx(const WidgetSize& size_px) {
    size_px_ = size_px;

    container_->SetHeight(size_px.height, true)
        .SetWidth(size_px.width, true);
}

} // namespace eerie_leap::views::widgets
