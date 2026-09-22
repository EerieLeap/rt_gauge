#include <algorithm>
#include <array>
#include <exception>
#include <stdexcept>
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

using eerie_leap::domain::ui_domain::utilities::WidgetPropertyValidator;
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
        .SetProcessingParent(parent_)
        .SetWidth(100, false)
        .SetHeight(100, false)
        .Build());
    container_->SetProcessingEnabled(false);

    content_frame_ = std::make_shared<Frame>(Frame::CreatePresentation(container_->GetObject())
        .SetProcessingParent(container_)
        .Build());
    container_->SetChild(content_frame_);

    transform_.Attach(id_, content_frame_, container_, *properties_, {
        .is_ready = [](void* context) { return static_cast<WidgetBase*>(context)->IsReady(); },
        .is_processing_eligible = [](void* context) {
            return static_cast<WidgetBase*>(context)->IsProcessingEligible();
        }
    }, this);

    lv_display_add_event_cb(lv_obj_get_display(container_->GetObject()), RefreshCallback, LV_EVENT_REFR_START, this);
}

WidgetBase::~WidgetBase() {
    DetachDispatch();

    // Before any other member goes: each entry unsubscribes as it is destroyed.
    subscriptions_.clear();
}

void WidgetBase::DetachDispatch() {
    ScopedLvglLock lvgl_guard;

    transform_.Detach();
    lv_display_remove_event_cb_with_user_data(lv_obj_get_display(container_->GetObject()), RefreshCallback, this);
    dispatch_guard_->Detach();
}

int WidgetBase::Render() {
    ScopedLvglLock lvgl_guard;

    is_ready_ = false;
    transform_.BeforeRender();
    UpdateProcessingState();

    const int result = RenderableBase::Render();
    if(result == 0)
        transform_.OnRendered();
    UpdateProcessingState();
    if(result == 0)
        ReplayPendingProperties();
    return result;
}

void WidgetBase::AddSubscription(AnySubscription subscription) {
    if(subscription != nullptr)
        subscriptions_.push_back(std::move(subscription));
}

uint32_t WidgetBase::GetId() const {
    return id_;
}

bool WidgetBase::IsActive() const {
    return properties_->GetAs<bool>(WidgetPropertyType::IS_ACTIVE, true);
}

bool WidgetBase::IsTrackingEligible() const {
    ScopedLvglLock lvgl_guard;
    return IsActive() && parent_->IsTrackingEnabled();
}

bool WidgetBase::IsProcessingEligible() const {
    ScopedLvglLock lvgl_guard;

    if(!is_group_active_ || !IsActive() || !IsVisible()
        || properties_->GetAs<int>(WidgetPropertyType::OPACITY, 255) == 0
        || !parent_->IsProcessingEnabled())
        return false;

    return Frame::IsVisibleInHierarchy(content_frame_->GetObject());
}

bool WidgetBase::IsAnimationEligible() const {
    ScopedLvglLock lvgl_guard;
    return IsReady() && IsProcessingEligible();
}

void WidgetBase::OnActivated() {
    ScopedLvglLock lvgl_guard;

    is_group_active_ = true;
    UpdateProcessingState();

    if(IsReady() && IsProcessingEligible())
        ReplayProperties();

    for(auto* dependency : dependencies_)
        dependency->OnActivated();
}

void WidgetBase::OnDeactivated() {
    ScopedLvglLock lvgl_guard;

    is_group_active_ = false;
    UpdateProcessingState();
    for(auto* dependency : dependencies_)
        dependency->OnDeactivated();
}

void WidgetBase::RegisterProperties(WidgetPropertyStore& store) const {
    store.Register(WidgetPropertyType::IS_ACTIVE, ConfigValue { true }, PropertyChangeEffect::None);
    store.Register(WidgetPropertyType::IS_VISIBLE, ConfigValue { true }, PropertyChangeEffect::None);
    store.Register(WidgetPropertyType::OPACITY, ConfigValue { 255 }, PropertyChangeEffect::None);
    store.Register(WidgetPropertyType::IS_SMOOTHED, ConfigValue { false }, PropertyChangeEffect::None);
    WidgetTransform::RegisterProperties(store);
}

bool WidgetBase::SetRotation(int32_t angle) {
    return transform_.SetRotation(angle);
}

void WidgetBase::OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) {
    if(type == WidgetPropertyType::IS_VISIBLE)
        SetVisibility(ConfigValueAs<bool>(value, true));
    else if(type == WidgetPropertyType::OPACITY)
        // Composite the children before fading, so overlapping parts fade together once.
        lv_obj_set_style_opa_layered(container_->GetObject(), ConfigValueAs<int>(value, 255), LV_PART_MAIN);
    else if(WidgetPropertyValidator::IsColorProperty(type) && IsReady())
        ApplyTheme(ThemeManager::GetInstance().GetCurrentTheme());
}

void WidgetBase::ApplyProperty(WidgetPropertyType type, const ConfigValue& value) {
    if(WidgetPropertyValidator::IsManagementProperty(type)) {
        WidgetBase::OnPropertyChanged(type, value);
        UpdateProcessingState();
    } else if(!transform_.ApplyProperty(type, value)) {
        properties_->ApplyColor(type);
        OnPropertyChanged(type, value);
        if(properties_->GetEffect(type) != PropertyChangeEffect::None)
            transform_.UpdateAnchor();
    }
}

void WidgetBase::UpdateProcessingState() {
    container_->SetTrackingEnabled(IsActive());
    container_->SetProcessingEnabled(is_group_active_ && IsActive() && IsVisible()
        && properties_->GetAs<int>(WidgetPropertyType::OPACITY, 255) > 0);
    const bool enabled = IsReady() && IsProcessingEligible();
    if(was_processing_ && !enabled)
        OnProcessingSuspended();
    was_processing_ = enabled;
    OnProcessingUpdated(enabled);
    transform_.OnProcessingUpdated(enabled);

    for(auto* dependency : dependencies_)
        dependency->UpdateProcessingState();

    if(!enabled)
        transform_.Synchronize();
}

void WidgetBase::OnProcessingSuspended() { }

void WidgetBase::OnProcessingUpdated(bool) { }

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
    if(WidgetPropertyValidator::IsManagementProperty(type)) {
        ApplyProperty(type, value);
        RunEffect(effect);
    } else if(pending_properties_.none() && IsReady() && IsProcessingEligible()) {
        ApplyProperty(type, value);
        RunEffect(effect);
        transform_.Synchronize();
        return;
    } else
        pending_properties_.set(static_cast<size_t>(type));

    ReplayPendingProperties();
}

void WidgetBase::SetPropertyLocal(WidgetPropertyType type, const ConfigValue& value) {
    static constexpr auto caller = GetCallerName();
    ScopedLvglLock lvgl_guard;

    if(WidgetPropertyValidator::IsStructuralProperty(type))
        throw std::invalid_argument("Structural properties require screen reconstruction, not a runtime update.");

    if(!IsProcessingEligible() || !properties_->Set(type, value))
        return;

    if(WidgetPropertyValidator::IsAnchorProperty(type))
        NotifyPropertyChanged(type, value, properties_->GetEffect(type));

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
    for(const auto& binding : configuration_->bindings) {
        if(WidgetPropertyValidator::IsStructuralProperty(binding.target))
            throw std::invalid_argument("Bindings cannot target structural properties.");
    }

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
            [
                this,
                store = properties_,
                guard = dispatch_guard_,
                target = binding.target,
                effect = properties_->GetEffect(binding.target),
                payload_key = binding.payload_key
            ](const ErasedPayloadView& view) {
                const auto* data = view.Find(payload_key);
                if(data == nullptr)
                    return;

                ScopedLvglLock lvgl_guard;

                guard->Dispatch([&] {
                    if(!WidgetPropertyValidator::IsManagementProperty(target) && !IsTrackingEligible())
                        return;

                    auto value = CoerceToConfigValue(*data, store->GetDeclaredAlternative(target), target);
                    if(std::holds_alternative<std::monostate>(value) || !store->Set(target, value))
                        return;

                    NotifyPropertyChanged(target, value, effect);
                });
            }));
    }
}

void WidgetBase::ReplayProperties() {
    PropertySet selected;
    const auto registered = properties_->GetRegisteredTypes();
    for(auto type : registered)
        selected.set(static_cast<size_t>(type));

    pending_properties_.reset();
    ApplyProperties(selected);

    // Configuration seeds members before rendering. Those handlers cannot necessarily apply
    // their visual state yet, and activation may still be blocked by visibility or opacity.
    if(!IsReady() || !IsProcessingEligible()) {
        for(auto type : registered) {
            if(!WidgetPropertyValidator::IsManagementProperty(type))
                pending_properties_.set(static_cast<size_t>(type));
        }
    }
}

void WidgetBase::ApplyProperties(const PropertySet& selected) {
    auto strongest = PropertyChangeEffect::None;
    auto apply = [&](WidgetPropertyType type) {
        if(!selected.test(static_cast<size_t>(type)))
            return;

        ApplyProperty(type, properties_->Get(type));
        strongest = std::max(strongest, properties_->GetEffect(type));
    };

    // VALUE consumes state declared by derived classes too (for example digital precision).
    // Registration order alone cannot express that dependency. Apply the value once, after
    // configuration, so charts still append at most one sample per replay.
    for(auto type : properties_->GetRegisteredTypes()) {
        if(type != WidgetPropertyType::VALUE)
            apply(type);
    }
    apply(WidgetPropertyType::VALUE);

    RunEffect(strongest);
    transform_.Synchronize();
}

void WidgetBase::ReplayPendingProperties() {
    if(!IsReady() || !IsProcessingEligible())
        return;

    transform_.UpdateAnchor();
    if(pending_properties_.any())
        ApplyProperties(std::exchange(pending_properties_, {}));
    else
        transform_.Synchronize();
}

void WidgetBase::RefreshCallback(lv_event_t* event) {
    ScopedLvglLock lvgl_guard;

    auto* widget = static_cast<WidgetBase*>(lv_event_get_user_data(event));
    widget->UpdateProcessingState();
    widget->ReplayPendingProperties();
    widget->transform_.UpdateAnchor();
}

void WidgetBase::Configure(std::shared_ptr<WidgetConfiguration> configuration) {
    ApplyConfiguration(std::move(configuration), true);
}

void WidgetBase::ConfigureAsPart(std::shared_ptr<WidgetConfiguration> configuration) {
    ApplyConfiguration(std::move(configuration), false);
}

void WidgetBase::ApplyConfiguration(std::shared_ptr<WidgetConfiguration> configuration, bool is_owner) {
    ScopedLvglLock lvgl_guard;

    configuration_ = std::move(configuration);
    transform_.Configure(*configuration_, is_owner);

    RegisterProperties(*properties_);

    // A dependency reads its own store, so its properties are configurable here but legitimately
    // absent from this one.
    auto supported = is_owner ? GetSupportedProperties() : std::vector<WidgetPropertyType> { };

    for(const auto& [type, value] : configuration_->properties) {
        if(WidgetPropertyValidator::IsStructuralProperty(type))
            continue;

        // Parts inherit their owner's management state through the Frame parent. Copying those
        // flags would leave a part independently hidden/inactive after its owner is restored.
        if(!is_owner && (WidgetPropertyValidator::IsManagementProperty(type)
            || WidgetPropertyValidator::IsAnimationProperty(type)))
            continue;

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
    ScopedLvglLock lvgl_guard;

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
    ScopedLvglLock lvgl_guard;

    size_px_ = size_px;

    container_->SetHeight(size_px.height, true)
        .SetWidth(size_px.width, true);
    transform_.UpdateAnchor();
}

} // namespace eerie_leap::views::widgets
