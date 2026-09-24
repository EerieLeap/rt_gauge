#include <algorithm>
#include <array>
#include <cerrno>
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

void WidgetBase::ValidateChildren(
    const WidgetConfiguration&,
    std::span<const WidgetConfiguration* const> children
) {
    if(!children.empty()) {
        throw std::invalid_argument("Leaf widget expects 0 children; child index 0, ID "
            + std::to_string(children.front()->id) + " is not allowed.");
    }
}

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
    ScopedLvglLock lvgl_guard;

    DetachDispatch();
    // Destroy descendants under the LVGL lock while their mounts are still alive.
    children_.clear();
}

void WidgetBase::Detach() {
    DetachDispatch();
}

void WidgetBase::DetachDispatch() {
    ScopedLvglLock lvgl_guard;

    if(std::exchange(detached_, true))
        return;

    is_ready_ = false;
    is_group_active_ = false;
    container_->SetTrackingEnabled(false);
    container_->SetProcessingEnabled(false);
    content_frame_->SetProcessingEnabled(false);
    OnProcessingSuspended();
    OnProcessingUpdated(false);
    was_processing_ = false;
    transform_.Detach();
    lv_obj_remove_event_cb_with_user_data(content_frame_->GetObject(), ChildMountGeometryCallback, this);
    lv_display_remove_event_cb_with_user_data(lv_obj_get_display(container_->GetObject()), RefreshCallback, this);
    ThemeManager::GetInstance().UnregisterObserver(this);
    dispatch_guard_->Detach();
    subscriptions_.clear();
    outbound_bindings_.clear();
    for(auto& child : children_)
        child->Detach();
}

std::shared_ptr<Frame> WidgetBase::GetChildMount() const {
    return content_frame_;
}

std::span<const std::unique_ptr<IWidget>> WidgetBase::GetChildren() const {
    return children_;
}

void WidgetBase::SetChildren(Children children) {
    ScopedLvglLock lvgl_guard;

    if(detached_ || children_injected_ || configuration_ != nullptr || IsReady())
        throw std::logic_error("Children must be injected once, before configuring the owner.");

    for(size_t i = 0; i < children.size(); ++i) {
        const auto& child = children[i];
        if(child == nullptr || child.get() == this)
            throw std::invalid_argument("An owned child must be a distinct widget instance.");

        const auto configuration = child->GetConfiguration();
        if(configuration == nullptr || configuration->id != child->GetId() || configuration->type != child->GetType())
            throw std::invalid_argument("Each child must be independently configured with its own identity.");

        if(lv_obj_get_parent(child->GetContainer()->GetObject()) != content_frame_->GetObject())
            throw std::invalid_argument("Children must be constructed under the owner's child mount.");

        for(size_t j = 0; j < i; ++j) {
            if(children[j]->GetId() == child->GetId())
                throw std::invalid_argument("Owned children must have distinct IDs.");
        }
    }

    OnChildrenAttached(children);
    children_ = std::move(children);
    children_injected_ = true;

    if(!children_.empty())
        lv_obj_add_event_cb(content_frame_->GetObject(), ChildMountGeometryCallback, LV_EVENT_SIZE_CHANGED, this);

    for(auto& child : children_)
        child->OnParentAttached();

    UpdateChildrenLayout();
}

void WidgetBase::OnChildrenAttached(std::span<const std::unique_ptr<IWidget>> children) {
    if(!children.empty())
        throw std::invalid_argument("This widget does not accept owned children.");
}

void WidgetBase::OnParentAttached() {
    ScopedLvglLock lvgl_guard;
    is_owned_ = true;
    lv_display_remove_event_cb_with_user_data(lv_obj_get_display(container_->GetObject()), RefreshCallback, this);
}

void WidgetBase::LayoutChildren() { }

void WidgetBase::UpdateChildrenLayout() {
    if(detached_ || children_.empty() || laying_out_children_)
        return;
    laying_out_children_ = true;
    try {
        lv_obj_update_layout(content_frame_->GetObject());
        LayoutChildren();
    } catch(...) {
        laying_out_children_ = false;
        throw;
    }
    laying_out_children_ = false;
}

void WidgetBase::ChildMountGeometryCallback(lv_event_t* event) {
    ScopedLvglLock lvgl_guard;
    auto* widget = static_cast<WidgetBase*>(lv_event_get_user_data(event));
    try {
        widget->UpdateChildrenLayout();
    } catch(const std::exception& error) {
        LOG_ERR("Widget %u child layout failed: %s", widget->id_, error.what());
    } catch(...) {
        LOG_ERR("Widget %u child layout failed.", widget->id_);
    }
}

int WidgetBase::Render() {
    ScopedLvglLock lvgl_guard;

    if(detached_)
        return -EINVAL;

    is_ready_ = false;
    transform_.BeforeRender();
    UpdateProcessingState();
    int result = 0;
    try {
        UpdateChildrenLayout();
        for(auto& child : children_) {
            result = child->Render();
            if(result != 0)
                break;
        }
        if(result == 0)
            result = RenderableBase::Render();
        if(result == 0)
            transform_.OnRendered();
        UpdateProcessingState();
        if(result == 0)
            ReplayPendingProperties();
    } catch(...) {
        is_ready_ = false;
        UpdateProcessingState();
        if(!is_owned_ || parent_->IsProcessingEnabled()) {
            for(auto& child : children_)
                child->Synchronize();
        }
        throw;
    }
    // During recursive rendering, the root releases the processing gate and
    // synchronizes the whole subtree once, including siblings after a failure.
    if(!is_owned_ || parent_->IsProcessingEnabled()) {
        for(auto& child : children_)
            child->Synchronize();
    }
    return result;
}

void WidgetBase::Synchronize() {
    ScopedLvglLock lvgl_guard;

    if(detached_)
        return;

    UpdateProcessingState();
    UpdateChildrenLayout();
    ReplayPendingProperties();
    transform_.UpdateAnchor();

    for(auto& child : children_)
        child->Synchronize();
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
    return !detached_ && IsActive() && parent_->IsTrackingEnabled();
}

bool WidgetBase::IsProcessingEligible() const {
    ScopedLvglLock lvgl_guard;

    if(detached_ || !is_group_active_ || !IsActive() || !IsVisible()
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

    if(detached_)
        return;

    is_group_active_ = true;
    UpdateProcessingState();

    if(IsReady() && IsProcessingEligible())
        ReplayProperties();

    for(auto& child : children_)
        child->OnActivated();
}

void WidgetBase::OnDeactivated() {
    ScopedLvglLock lvgl_guard;

    if(detached_)
        return;

    is_group_active_ = false;
    UpdateProcessingState();

    for(auto& child : children_)
        child->OnDeactivated();
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
    if(detached_)
        return;

    // A partially rendered composite must not let descendants process or animate.
    content_frame_->SetProcessingEnabled(children_.empty() || IsReady());
    container_->SetTrackingEnabled(IsActive());
    container_->SetProcessingEnabled(is_group_active_ && IsActive() && IsVisible()
        && properties_->GetAs<int>(WidgetPropertyType::OPACITY, 255) > 0);
    const bool enabled = IsReady() && IsProcessingEligible();
    if(was_processing_ && !enabled)
        OnProcessingSuspended();
    was_processing_ = enabled;
    OnProcessingUpdated(enabled);
    transform_.OnProcessingUpdated(enabled);

    if(!enabled)
        transform_.Synchronize();
}

void WidgetBase::OnProcessingSuspended() { }

void WidgetBase::OnProcessingUpdated(bool) { }

void WidgetBase::OnConfigured() { }

std::vector<WidgetPropertyType> WidgetBase::GetSupportedProperties() const {
    WidgetPropertyStore declared;

    RegisterProperties(declared);

    return declared.GetRegisteredTypes();
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

    if(WidgetPropertyValidator::IsManagementProperty(type)) {
        for(auto& child : children_)
            child->Synchronize();
    }
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
    widget->Synchronize();
}

void WidgetBase::Configure(std::shared_ptr<WidgetConfiguration> configuration) {
    ScopedLvglLock lvgl_guard;

    if(detached_)
        throw std::logic_error("A detached widget cannot be configured.");

    if((children_injected_ || is_owned_) && configuration_ != nullptr)
        throw std::logic_error("Owned composition requires reconstruction to change configuration.");

    if(children_injected_) {
        if(configuration == nullptr || configuration->id != id_ || configuration->type != GetType())
            throw std::invalid_argument("The owner must retain its configured identity.");

        const auto it = configuration->properties.find(WidgetPropertyType::CHILD_WIDGET_IDS);
        const auto* ids = it != configuration->properties.end()
            ? std::get_if<std::pmr::vector<int>>(&it->second)
            : nullptr;

        if((ids == nullptr && !children_.empty()) || (ids != nullptr && ids->size() != children_.size()))
            throw std::invalid_argument("Injected children must match CHILD_WIDGET_IDS in order.");

        for(size_t i = 0; i < children_.size(); ++i) {
            if((*ids)[i] < 0 || static_cast<uint32_t>((*ids)[i]) != children_[i]->GetId())
                throw std::invalid_argument("Injected children must match CHILD_WIDGET_IDS in order.");
        }
    } else {
        OnChildrenAttached({});
    }

    configuration_ = std::move(configuration);
    transform_.Configure(*configuration_);

    RegisterProperties(*properties_);

    const auto supported = GetSupportedProperties();

    for(const auto& [type, value] : configuration_->properties) {
        if(WidgetPropertyValidator::IsStructuralProperty(type))
            continue;

        if(!properties_->Set(type, value)
            && std::find(supported.begin(), supported.end(), type) == supported.end())
            LOG_WRN("Widget %u does not support property %u.", id_, static_cast<unsigned>(type));
    }

    ReplayProperties();
    OnConfigured();
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

    UpdateChildrenLayout();
    transform_.UpdateAnchor();
}

} // namespace eerie_leap::views::widgets
