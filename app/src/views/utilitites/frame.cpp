#include <stdexcept>
#include <utility>

#include "domain/ui_domain/lvgl_lock.h"

#include "frame.h"

namespace eerie_leap::views::utilitites {

using eerie_leap::domain::ui_domain::ScopedLvglLock;

namespace {

// Reserved by Frame for widget presentation nodes; do not reuse for application state.
constexpr lv_obj_flag_t presentation_flag = LV_OBJ_FLAG_USER_1;

} // namespace

Frame::Frame() : lv_object_(nullptr) { }

Frame::~Frame() {
    child_.reset();

    if(lv_object_ != nullptr) {
        lv_obj_delete(lv_object_);
        lv_object_ = nullptr;
    }
}

Frame::Frame(Frame&& other) noexcept
        : lv_object_(other.lv_object_),
        child_(std::move(other.child_)),
        processing_parent_(std::move(other.processing_parent_)),
        is_processing_enabled_(other.is_processing_enabled_),
        is_tracking_enabled_(other.is_tracking_enabled_) {

    other.lv_object_ = nullptr;
}

Frame& Frame::operator=(Frame&& other) noexcept {
    if(this == &other)
        return *this;

    child_.reset();

    if(lv_object_ != nullptr)
        lv_obj_delete(lv_object_);

    lv_object_ = other.lv_object_;
    child_ = std::move(other.child_);
    processing_parent_ = std::move(other.processing_parent_);
    is_processing_enabled_ = other.is_processing_enabled_;
    is_tracking_enabled_ = other.is_tracking_enabled_;
    other.lv_object_ = nullptr;

    return *this;
}

Frame Frame::Create(lv_obj_t* object) {
    Frame frame;
    frame.lv_object_ = object;

    return frame;
}

Frame Frame::CreateWrapped(lv_obj_t* object) {
    lv_obj_t* frame = lv_obj_create(object == nullptr ? lv_screen_active() : object);
    lv_obj_remove_style_all(frame);

    // A scrollable layout container makes LVGL treat drags as scrolling and never emits a gesture.
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);

    return Frame::Create(frame);
}

Frame Frame::Build() {
    Invalidate();

    return std::move(*this);
}

Frame Frame::CreatePresentation(lv_obj_t* parent) {
    auto frame = CreateWrapped(parent);
    lv_obj_add_flag(frame.lv_object_, static_cast<lv_obj_flag_t>(presentation_flag | LV_OBJ_FLAG_GESTURE_BUBBLE));
    lv_obj_remove_flag(frame.lv_object_, static_cast<lv_obj_flag_t>(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_CLICK_FOCUSABLE
        | LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_SCROLL_ON_FOCUS));
    lv_obj_set_style_transform_pivot_x(frame.lv_object_, lv_pct(50), LV_PART_MAIN);
    lv_obj_set_style_transform_pivot_y(frame.lv_object_, lv_pct(50), LV_PART_MAIN);
    frame.SetWidth(100, false).SetHeight(100, false);
    return frame;
}

bool Frame::IsVisibleInHierarchy(const lv_obj_t* object) {
    for(; object != nullptr; object = lv_obj_get_parent(object)) {
        if(lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN)
            || lv_obj_get_style_opa(object, LV_PART_MAIN) == LV_OPA_TRANSP
            || (!lv_obj_has_flag(object, presentation_flag)
                && lv_obj_get_style_opa_layered(object, LV_PART_MAIN) == LV_OPA_TRANSP))
            return false;
    }
    return true;
}

void Frame::ValidateFrame(const lv_obj_t* frame) {
    if(!frame)
        throw std::runtime_error("Frame not created");
}

Frame& Frame::Invalidate() {
    ValidateFrame(lv_object_);

    lv_obj_invalidate(lv_object_);

    return *this;
}

Frame& Frame::CleanStyles() {
    ValidateFrame(lv_object_);

    lv_obj_remove_style_all(lv_object_);

    return *this;
}

Frame& Frame::SetWidth(int32_t width, bool is_px) {
    ValidateFrame(lv_object_);

    lv_obj_set_width(lv_object_, is_px ? width : lv_pct(width));

    return *this;
}

Frame& Frame::SetHeight(int32_t height, bool is_px) {
    ValidateFrame(lv_object_);

    lv_obj_set_height(lv_object_, is_px ? height : lv_pct(height));

    return *this;
}

Frame& Frame::SetXOffset(int32_t offset, bool is_px) {
    ValidateFrame(lv_object_);

    lv_obj_set_x(lv_object_, is_px ? offset : lv_pct(offset));

    return *this;
}

Frame& Frame::SetYOffset(int32_t offset, bool is_px) {
    ValidateFrame(lv_object_);

    lv_obj_set_y(lv_object_, is_px ? offset : lv_pct(offset));

    return *this;
}

Frame& Frame::SetPaddingLeft(int32_t padding) {
    ValidateFrame(lv_object_);

    lv_obj_set_style_pad_left(lv_object_, padding, LV_PART_MAIN | LV_STATE_DEFAULT);

    return *this;
}

Frame& Frame::SetPaddingRight(int32_t padding) {
    ValidateFrame(lv_object_);

    lv_obj_set_style_pad_right(lv_object_, padding, LV_PART_MAIN | LV_STATE_DEFAULT);

    return *this;
}

Frame& Frame::SetPaddingTop(int32_t padding) {
    ValidateFrame(lv_object_);

    lv_obj_set_style_pad_top(lv_object_, padding, LV_PART_MAIN | LV_STATE_DEFAULT);

    return *this;
}

Frame& Frame::SetPaddingBottom(int32_t padding) {
    ValidateFrame(lv_object_);

    lv_obj_set_style_pad_bottom(lv_object_, padding, LV_PART_MAIN | LV_STATE_DEFAULT);

    return *this;
}

Frame& Frame::AlignBottom() {
    ValidateFrame(lv_object_);

    lv_obj_set_align(lv_object_, LV_ALIGN_BOTTOM_MID);

    return *this;
}

Frame& Frame::AlignTop() {
    ValidateFrame(lv_object_);

    lv_obj_set_align(lv_object_, LV_ALIGN_TOP_MID);

    return *this;
}

Frame& Frame::AlignLeft() {
    ValidateFrame(lv_object_);

    lv_obj_set_align(lv_object_, LV_ALIGN_LEFT_MID);

    return *this;
}

Frame& Frame::AlignRight() {
    ValidateFrame(lv_object_);

    lv_obj_set_align(lv_object_, LV_ALIGN_RIGHT_MID);

    return *this;
}

Frame& Frame::AlignCenter() {
    ValidateFrame(lv_object_);

    lv_obj_set_align(lv_object_, LV_ALIGN_CENTER);

    return *this;
}

lv_obj_t* Frame::GetObject() {
    return lv_object_;
}

Frame& Frame::SetProcessingParent(std::weak_ptr<Frame> parent) {
    ScopedLvglLock lvgl_guard;
    processing_parent_ = std::move(parent);
    return *this;
}

void Frame::SetProcessingEnabled(bool enabled) {
    ScopedLvglLock lvgl_guard;
    is_processing_enabled_ = enabled;
}

bool Frame::IsProcessingEnabled() const {
    ScopedLvglLock lvgl_guard;
    if(!is_processing_enabled_)
        return false;

    auto parent = processing_parent_.lock();
    return parent == nullptr || parent->IsProcessingEnabled();
}

void Frame::SetTrackingEnabled(bool enabled) {
    ScopedLvglLock lvgl_guard;
    is_tracking_enabled_ = enabled;
}

bool Frame::IsTrackingEnabled() const {
    ScopedLvglLock lvgl_guard;
    if(!is_tracking_enabled_)
        return false;

    auto parent = processing_parent_.lock();
    return parent == nullptr || parent->IsTrackingEnabled();
}

void Frame::SetChild(std::shared_ptr<Frame> child) {
    child_ = std::move(child);
}

std::shared_ptr<Frame> Frame::GetChild() {
    return child_;
}

} // namespace eerie_leap::views::utilitites
