#pragma once

#include <span>
#include <vector>

#include "views/widgets/indicators/indicator_base.h"

namespace eerie_leap::views::widgets::indicators {

class DialIndicator : public IndicatorBase {
private:
    int start_angle_;
    int end_angle_;

    static constexpr int DEFAULT_START_ANGLE = 45;
    static constexpr int DEFAULT_END_ANGLE = 315;

    // Child index 0, owned by WidgetBase; any widget type can serve as the needle.
    IWidget* needle_ = nullptr;

    void UpdateIndicator(float value) override;

    int DoRender() override;
    int ApplyTheme(const ITheme& theme) override;

protected:
    void RegisterProperties(WidgetPropertyStore& store) const override;
    void OnPropertyChanged(WidgetPropertyType type, const ConfigValue& value) override;
    void OnChildrenAttached(std::span<const std::unique_ptr<IWidget>> children) override;

public:

    uint32_t GetAngleForValue(float value);

public:
    // Index 0 is the driven needle. Its outer geometry is a fill-slot placeholder:
    // position (0, 0), size (1, 1); renderer dimensions/offsets/anchors stay local.
    static void ValidateChildren(const WidgetConfiguration& configuration,
        std::span<const WidgetConfiguration* const> children);

    explicit DialIndicator(uint32_t id, std::shared_ptr<Frame> parent, WidgetContext context);
    ~DialIndicator() override;

    std::vector<WidgetPropertyType> GetSupportedProperties() const override;

    [[nodiscard]] WidgetType GetType() const override { return WidgetType::IndicatorDial; }
};

} // namespace eerie_leap::views::widgets::indicators
