#include <cerrno>
#include <cstdlib>
#include <stdexcept>
#include <string>

#include "domain/ui_domain/models/widget_property.h"

#include "views/widgets/indicators/indicator_base.h"

#include "dial_indicator.h"

namespace eerie_leap::views::widgets::indicators {

using namespace eerie_leap::utilities::type;
using namespace eerie_leap::domain::ui_domain::models;
using namespace eerie_leap::views::utilitites;

namespace {

std::string NeedleCountError(size_t count) {
    return "Dial expects exactly 1 child: index 0 is the needle; received " + std::to_string(count) + ".";
}

std::string NeedleError(uint32_t id, const char* reason) {
    return "Child index 0, ID " + std::to_string(id) + " (needle): " + reason;
}

constexpr const char* rotation_conflict =
    "driven rotation conflicts with a rotation animation or inbound ANIMATION_TYPE binding.";

} // namespace

void DialIndicator::ValidateChildren(const WidgetConfiguration&,
    std::span<const WidgetConfiguration* const> children) {
    if(children.size() != 1)
        throw std::invalid_argument(NeedleCountError(children.size()));

    const auto& needle = *children.front();
    if(needle.position_grid.x != 0 || needle.position_grid.y != 0
        || needle.size_grid.width != 1 || needle.size_grid.height != 1)
        throw std::invalid_argument(NeedleError(needle.id, "fill slot requires position (0, 0) and size (1, 1)."));
    if(!WidgetTransform::CanSetRotation(needle))
        throw std::invalid_argument(NeedleError(needle.id, rotation_conflict));
}

DialIndicator::DialIndicator(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context)
    : IndicatorBase(id, std::move(parent), std::move(context)) {}

DialIndicator::~DialIndicator() {
    DetachDispatch();
}

void DialIndicator::OnChildrenAttached(std::span<const std::unique_ptr<IWidget>> children) {
    needle_ = children.empty() ? nullptr : children.front().get();
}

std::vector<WidgetPropertyType> DialIndicator::GetSupportedProperties() const {
    auto supported = IndicatorBase::GetSupportedProperties();
    supported.push_back(WidgetPropertyType::CHILD_WIDGET_IDS);
    return supported;
}

int DialIndicator::DoRender() {
    // Validation rejects a dial without its needle; an unvalidated one fails only this widget.
    if(needle_ == nullptr)
        return -EINVAL;

    // Children render before their owner, so the needle already exists.
    UpdateIndicator(range_start_);

    return 0;
}

int DialIndicator::ApplyTheme(const ITheme&) {
    return 0;
}

uint32_t DialIndicator::GetAngleForValue(float value) {
    float resolution = (abs(end_angle_ - start_angle_) * 10) / abs(range_end_ - range_start_);
    float angle_delta = resolution * value;

    return start_angle_ * 10 - 1800 + static_cast<uint32_t>(angle_delta);
}

void DialIndicator::UpdateIndicator(float value) {
    // Angles below zero wrap as unsigned here; SetRotation normalizes full turns.
    needle_->SetRotation(static_cast<int32_t>(GetAngleForValue(value)));
}

void DialIndicator::RegisterProperties(WidgetPropertyStore& store) const {
    IndicatorBase::RegisterProperties(store);

    store.Register(WidgetPropertyType::START_ANGLE, ConfigValue { DEFAULT_START_ANGLE }, PropertyChangeEffect::Repaint);
    store.Register(WidgetPropertyType::END_ANGLE, ConfigValue { DEFAULT_END_ANGLE }, PropertyChangeEffect::Repaint);
}

void DialIndicator::OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) {
    switch(type) {
        case WidgetPropertyType::START_ANGLE:
            start_angle_ = ConfigValueAs<int>(value, DEFAULT_START_ANGLE);
            break;

        case WidgetPropertyType::END_ANGLE:
            end_angle_ = ConfigValueAs<int>(value, DEFAULT_END_ANGLE);
            break;

        default:
            IndicatorBase::OnPropertyChanged(type, value);
            break;
    }
}

} // namespace eerie_leap::views::widgets::indicators
