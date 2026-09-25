#include <zephyr/logging/log.h>

#include "domain/ui_domain/lvgl_lock.h"
#include "widget_transform.h"

namespace eerie_leap::views::widgets {

using namespace eerie_leap::domain::ui_domain::models;
using eerie_leap::domain::ui_domain::ScopedLvglLock;
using eerie_leap::domain::ui_domain::utilities::WidgetPropertyValidator;

LOG_MODULE_REGISTER(widget_transform_logger);

static_assert(WidgetPropertyValidator::max_anchor_coordinate == LV_COORD_MAX);

WidgetTransform::~WidgetTransform() {
    Detach();
}

bool WidgetTransform::Attach(
    uint32_t id,
    std::shared_ptr<Frame> presentation,
    std::shared_ptr<Frame> layout,
    Callbacks callbacks,
    void* context
) {
    if(presentation == nullptr || layout == nullptr
        || presentation->GetObject() == nullptr || layout->GetObject() == nullptr
        || callbacks.is_ready == nullptr || callbacks.is_processing_eligible == nullptr
    ) {
        return false;
    }

    Detach();

    id_ = id;
    presentation_ = std::move(presentation);
    layout_ = std::move(layout);
    callbacks_ = callbacks;
    context_ = context;
    if(!animation_.Attach(*presentation_, *layout_, [](void* context) {
        const auto* transform = static_cast<WidgetTransform*>(context);
        return transform->callbacks_.is_ready(transform->context_)
            && transform->callbacks_.is_processing_eligible(transform->context_);
    }, this)) {
        Detach();
        return false;
    }
    lv_obj_add_event_cb(presentation_->GetObject(), AnchorGeometryCallback, LV_EVENT_SIZE_CHANGED, this);

    return true;
}

void WidgetTransform::Configure(const WidgetConfiguration& configuration) {
    rotation_allowed_ = CanSetRotation(configuration);
}

void WidgetTransform::Detach() {
    ScopedLvglLock lock;

    SetTargetFrame(nullptr);

    if(presentation_ != nullptr)
        lv_obj_remove_event_cb_with_user_data(presentation_->GetObject(), AnchorGeometryCallback, this);

    animation_.Detach();
    presentation_ = nullptr;
    layout_ = nullptr;
    callbacks_ = {};
    context_ = nullptr;
}

void WidgetTransform::RegisterProperties(WidgetPropertyStore& store) {
    store.Register(WidgetPropertyType::ANCHOR_POINT_X, ConfigValue { -1 }, PropertyChangeEffect::Repaint);
    store.Register(WidgetPropertyType::ANCHOR_POINT_Y, ConfigValue { -1 }, PropertyChangeEffect::Repaint);
    WidgetAnimation::RegisterProperties(store);
}

bool WidgetTransform::ApplyProperty(WidgetPropertyType type, const ConfigValue& value) {
    if(animation_.ApplyProperty(type, value)) {
        if(type == WidgetPropertyType::ANIMATION_TYPE)
            UpdateAnchor();

        return true;
    }

    if(type == WidgetPropertyType::ANCHOR_POINT_X)
        anchor_point_.x = std::get<int>(value);
    else if(type == WidgetPropertyType::ANCHOR_POINT_Y)
        anchor_point_.y = std::get<int>(value);

    return false;
}

void WidgetTransform::Synchronize() {
    animation_.Synchronize();
}

void WidgetTransform::BeforeRender() {
    SetTargetFrame(nullptr);
    animation_.StopAndReset();
}

void WidgetTransform::OnRendered() {
    target_pending_ = true;
    UpdateAnchor();
}

void WidgetTransform::OnProcessingUpdated(bool enabled) {
    if(enabled && angle_.has_value())
        UpdateAnchor();
}

namespace {

bool HasRotationBinding(const WidgetConfiguration& configuration) {
    for(const auto& binding : configuration.bindings) {
        // An inbound type binding could select a competing rotation later.
        if(binding.target == WidgetPropertyType::ANIMATION_TYPE
            && binding.direction != PropertyBindingDirection::Out)
            return true;
    }
    return false;
}

} // namespace

bool WidgetTransform::CanSetRotation(const WidgetConfiguration& configuration) {
    const auto it = configuration.properties.find(WidgetPropertyType::ANIMATION_TYPE);
    if(it != configuration.properties.end()) {
        const auto* type = std::get_if<int>(&it->second);
        if(type == nullptr || *type == static_cast<int>(Animation::Type::Rotation))
            return false;
    }
    return !HasRotationBinding(configuration);
}

bool WidgetTransform::SetRotation(int32_t angle) {
    ScopedLvglLock lock;

    if(presentation_ == nullptr || !rotation_allowed_)
        return false;

    angle_ = (angle % 3600 + 3600) % 3600;
    if(callbacks_.is_ready(context_) && callbacks_.is_processing_eligible(context_))
        UpdateAnchor();

    return true;
}

void WidgetTransform::SetTargetFrame(std::shared_ptr<Frame> target) {
    if(target_ != nullptr && target_ != presentation_)
        lv_obj_remove_event_cb_with_user_data(target_->GetObject(), AnchorGeometryCallback, this);
    target_ = std::move(target);
    if(target_ != nullptr && target_ != presentation_)
        lv_obj_add_event_cb(target_->GetObject(), AnchorGeometryCallback, LV_EVENT_SIZE_CHANGED, this);
}

void WidgetTransform::AnchorGeometryCallback(lv_event_t* event) {
    ScopedLvglLock lvgl_guard;

    static_cast<WidgetTransform*>(lv_event_get_user_data(event))->UpdateAnchor();
}

void WidgetTransform::UpdateAnchor() {
    if(presentation_ == nullptr || !callbacks_.is_ready(context_) || updating_anchor_)
        return;

    // Anchors only place rotation, so a widget that never rotates skips the layout work.
    if(!angle_.has_value() && !animation_.IsRotation()) {
        target_pending_ = false;
        return;
    }

    auto* presentation = presentation_->GetObject();
    auto* bounds = target_ != nullptr ? target_->GetObject() : presentation;

    updating_anchor_ = true;
    lv_obj_update_layout(presentation);
    const lv_point_t local {
        anchor_point_.x == -1 ? lv_obj_get_width(bounds) / 2 : anchor_point_.x,
        anchor_point_.y == -1 ? lv_obj_get_height(bounds) / 2 : lv_obj_get_height(bounds) - anchor_point_.y
    };

    lv_area_t bounds_area;
    lv_area_t presentation_area;
    lv_obj_get_coords(bounds, &bounds_area);
    lv_obj_get_coords(presentation, &presentation_area);
    const int64_t translated_x = static_cast<int64_t>(local.x) + bounds_area.x1 - presentation_area.x1;
    const int64_t translated_y = static_cast<int64_t>(local.y) + bounds_area.y1 - presentation_area.y1;

    if(translated_x < LV_COORD_MIN || translated_x > LV_COORD_MAX
        || translated_y < LV_COORD_MIN || translated_y > LV_COORD_MAX) {
        LOG_WRN("Widget %u anchor cannot be represented in presentation coordinates.", id_);
        updating_anchor_ = false;
        return;
    }

    if(lv_obj_get_style_transform_pivot_x(presentation, LV_PART_MAIN) != translated_x)
        lv_obj_set_style_transform_pivot_x(presentation, static_cast<int32_t>(translated_x), LV_PART_MAIN);

    if(lv_obj_get_style_transform_pivot_y(presentation, LV_PART_MAIN) != translated_y)
        lv_obj_set_style_transform_pivot_y(presentation, static_cast<int32_t>(translated_y), LV_PART_MAIN);
    if(bounds != presentation) {
        if(lv_obj_get_style_transform_pivot_x(bounds, LV_PART_MAIN) != local.x)
            lv_obj_set_style_transform_pivot_x(bounds, local.x, LV_PART_MAIN);
        if(lv_obj_get_style_transform_pivot_y(bounds, LV_PART_MAIN) != local.y)
            lv_obj_set_style_transform_pivot_y(bounds, local.y, LV_PART_MAIN);
    }
    // Images and rasterized shapes share LVGL's native image transform, which
    // allows rotated pixels outside the drawable's original bounds. The pivot relies
    // on the local LVGL patch; a style transform instead leaves artifacts around images.
    const bool is_image = lv_obj_check_type(bounds, &lv_image_class);
    if(is_image)
        lv_image_set_pivot(bounds, local.x, local.y);

    // Explicit rendering initializes even an inactive staged target. Later updates
    // to an existing target wait for processing to resume.
    if(angle_.has_value() && rotation_allowed_ && (target_pending_ || callbacks_.is_processing_eligible(context_))) {
        if(is_image)
            lv_image_set_rotation(bounds, *angle_);
        else
            animation_.SetRotation(*angle_);
    }

    target_pending_ = false;
    lv_obj_refresh_ext_draw_size(layout_->GetObject());
    updating_anchor_ = false;
}

} // namespace eerie_leap::views::widgets
