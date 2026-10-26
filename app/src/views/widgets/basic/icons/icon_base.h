#pragma once

#include <lvgl.h>
#include <memory>

#include "views/renderable_base.h"

#include "i_icon.h"

namespace eerie_leap::views::widgets::basic::icons {

using eerie_leap::views::RenderableBase;

class IconBase : public RenderableBase, public IIcon {
protected:
    std::shared_ptr<Frame> parent_;
    std::shared_ptr<WidgetPropertyStore> properties_;
    bool is_processing_enabled_ = false;

    bool IsProcessingEligible() const;

    // What the owning widget was given, where an icon finds its files and fonts.
    WidgetContext context_;

    // The largest width or height an icon draws: keeps malformed configuration from becoming LVGL's encoded
    // percentage sizes or an unbounded allocation.
    static constexpr int MAX_DIMENSION = 32767;

    // A pixel property, safe for any stored or bound number: -1 for a dimension outside 0 to MAX_DIMENSION or not
    // finite, else (a thickness, a radius) clamped into that range, 0 when not finite.
    static int ReadPixels(const WidgetPropertyStore& store, WidgetPropertyType type, int fallback, bool is_dimension);

public:
    explicit IconBase(std::shared_ptr<Frame> parent);
    ~IconBase() override = default;

    // Factory metadata: declarations must be available without creating LVGL objects.
    static void RegisterProperties(WidgetPropertyStore&) {}

    void SetContext(WidgetContext context) override;

    void SetProcessingEnabled(bool enabled) override;
    void Configure(std::shared_ptr<WidgetPropertyStore> properties) override;
};

} // namespace eerie_leap::views::widgets::basic::icons
