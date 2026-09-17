#pragma once

#include <memory>

#include <lvgl.h>

namespace eerie_leap::views::utilitites {

// Owns the LVGL object it wraps and deletes it, so an lv_obj_t lives exactly as
// long as the C++ object that built it.
//
// That ownership is exclusive, which is why Frame is move-only: a copy would
// delete the same object twice. Every builder below hands ownership to its
// return value, so a chain like Frame::CreateWrapped(p).SetWidth(...).Build()
// leaves the result owning the object and the discarded temporaries owning
// nothing.
class Frame {
private:
    lv_obj_t* lv_object_;
    std::shared_ptr<Frame> child_;
    std::weak_ptr<Frame> processing_parent_;
    bool is_processing_enabled_ = true;
    bool is_tracking_enabled_ = true;

    Frame();

    static void ValidateFrame(const lv_obj_t* frame);

public:
    ~Frame();

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;
    Frame(Frame&& other) noexcept;
    Frame& operator=(Frame&& other) noexcept;

    static Frame Create(lv_obj_t* parent);
    static Frame CreateWrapped(lv_obj_t* parent = nullptr);

    Frame Build();
    Frame& Invalidate();
    Frame& CleanStyles();
    Frame& SetWidth(int32_t width, bool is_px);
    Frame& SetHeight(int32_t height, bool is_px);
    Frame& SetXOffset(int32_t offset, bool is_px);
    Frame& SetYOffset(int32_t offset, bool is_px);
    Frame& SetPaddingLeft(int32_t padding);
    Frame& SetPaddingRight(int32_t padding);
    Frame& SetPaddingTop(int32_t padding);
    Frame& SetPaddingBottom(int32_t padding);
    Frame& AlignBottom();
    Frame& AlignTop();
    Frame& AlignLeft();
    Frame& AlignRight();
    Frame& AlignCenter();
    Frame& SetProcessingParent(std::weak_ptr<Frame> parent);
    void SetProcessingEnabled(bool enabled);
    bool IsProcessingEnabled() const;
    void SetTrackingEnabled(bool enabled);
    bool IsTrackingEnabled() const;

    lv_obj_t* GetObject();
    void SetChild(std::shared_ptr<Frame> child);
    std::shared_ptr<Frame> GetChild();
};

} // namespace eerie_leap::views::utilitites
