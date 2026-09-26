#pragma once

#include <cstdint>

namespace eerie_leap::domain::ui_domain::models {

enum class WidgetCategory : uint32_t {
    None = 0,
    Basic = 1 << 16,
    Indicator = 2 << 16,
    Control = 3 << 16
};

// Persisted in the widget configuration - append only.
enum class WidgetType : uint32_t {
    None = 0,

    BasicIcon = static_cast<uint32_t>(WidgetCategory::Basic) | 1,
    BasicArcIcon = static_cast<uint32_t>(WidgetCategory::Basic) | 2,
    BasicLottie = static_cast<uint32_t>(WidgetCategory::Basic) | 3,

    IndicatorArcFill = static_cast<uint32_t>(WidgetCategory::Indicator) | 101,
    IndicatorDigital = static_cast<uint32_t>(WidgetCategory::Indicator) | 102,
    IndicatorHorizontalChart = static_cast<uint32_t>(WidgetCategory::Indicator) | 103,
    IndicatorSegmentArc = static_cast<uint32_t>(WidgetCategory::Indicator) | 104,
    IndicatorDial = static_cast<uint32_t>(WidgetCategory::Indicator) | 105,
    IndicatorBar = static_cast<uint32_t>(WidgetCategory::Indicator) | 106,
    IndicatorSetting = static_cast<uint32_t>(WidgetCategory::Indicator) | 107,

    ControlSlider = static_cast<uint32_t>(WidgetCategory::Control) | 201,
    ControlToggle = static_cast<uint32_t>(WidgetCategory::Control) | 202,
    ControlButton = static_cast<uint32_t>(WidgetCategory::Control) | 203,
};

class WidgetTypeHelpers {
private:
    static constexpr uint32_t category_mask_ = 0xFFFF0000U;

public:
    static constexpr WidgetCategory GetCategory(WidgetType widget_type) {
        return static_cast<WidgetCategory>(static_cast<uint32_t>(widget_type) & category_mask_);
    }

    // Control (3 << 16) contains the bits of Basic and Indicator, so the masked
    // halves have to be compared for equality rather than ANDed together.
    static constexpr bool IsCategory(WidgetType widget_type, WidgetCategory category) {
        return GetCategory(widget_type) == category;
    }
};

} // namespace eerie_leap::domain::ui_domain::models
