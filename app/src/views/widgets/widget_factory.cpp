#include <stdexcept>
#include <string>

#include "views/widgets/basic/icon_widget/icon_widget.h"
#include "views/widgets/basic/arc_icon_widget/arc_icon_widget.h"

#include "views/widgets/controls/button_control/button_control.h"
#include "views/widgets/controls/slider_control/slider_control.h"
#include "views/widgets/controls/toggle_control/toggle_control.h"

#include "views/widgets/indicators/arc_fill_indicator/arc_fill_indicator.h"
#include "views/widgets/indicators/segment_arc_indicator/segment_arc_indicator.h"
#include "views/widgets/indicators/digital_indicator/digital_indicator.h"
#include "views/widgets/indicators/horizontal_chart_indicator/horizontal_chart_indicator.h"
#include "views/widgets/indicators/dial_indicator/dial_indicator.h"
#include "views/widgets/indicators/bar_indicator/bar_indicator.h"
#include "views/widgets/indicators/setting_indicator/setting_indicator.h"

#include "widget_factory.h"

namespace eerie_leap::views::widgets {

using namespace eerie_leap::views::widgets::basic;
using namespace eerie_leap::views::widgets::controls;
using namespace eerie_leap::views::widgets::indicators;

WidgetFactory::WidgetFactory() {
    RegisterTypes();
}

WidgetFactory& WidgetFactory::GetInstance() {
    static WidgetFactory instance;
    return instance;
}

void WidgetFactory::RegisterWidget(const WidgetType type, WidgetCreator creator, ChildValidator validator) {
    registrations_[type] = { std::move(creator), validator ? std::move(validator) : WidgetBase::ValidateChildren };
}

void WidgetFactory::ValidateChildren(
    const WidgetConfiguration& owner,
    std::span<const WidgetConfiguration* const> children
) const {
    const auto it = registrations_.find(owner.type);
    if(it == registrations_.end())
        throw std::invalid_argument("Unknown widget type.");
    it->second.validate_children(owner, children);
}

std::unique_ptr<IWidget> WidgetFactory::CreateWidget(const WidgetType type, const uint32_t id, std::shared_ptr<Frame> parent, const WidgetContext& context) {
    auto it = registrations_.find(type);
    if (it == registrations_.end())
        throw std::runtime_error("Unknown widget type");

    return it->second.create(id, std::move(parent), context);
}

std::unique_ptr<IWidget> WidgetFactory::CreateWidget(std::shared_ptr<WidgetConfiguration> configuration, std::shared_ptr<Frame> parent, const WidgetContext& context) {
    auto widget = CreateWidget(configuration->type, configuration->id, std::move(parent), context);
    ConfigureWidget(*widget, std::move(configuration), {});

    return widget;
}

void WidgetFactory::ConfigureWidget(IWidget& widget, std::shared_ptr<WidgetConfiguration> configuration, IWidget::Children children) const {
    // Injection fixes a widget's configuration, so childless widgets skip it and stay reconfigurable.
    if(!children.empty())
        widget.SetChildren(std::move(children));

    widget.Configure(std::move(configuration));
}

std::vector<WidgetType> WidgetFactory::GetAvailableTypes() const {
    std::vector<WidgetType> types;
    types.reserve(registrations_.size());

    for (const auto& [type, registration] : registrations_)
        types.push_back(type);

    return types;
}

void WidgetFactory::RegisterTypes() {
    RegisterWidget<IconWidget>(WidgetType::BasicIcon);
    RegisterWidget<ArcIconWidget>(WidgetType::BasicArcIcon);

    RegisterWidget<ArcFillIndicator>(WidgetType::IndicatorArcFill);
    RegisterWidget<SegmentArcIndicator>(WidgetType::IndicatorSegmentArc);
    RegisterWidget<DigitalIndicator>(WidgetType::IndicatorDigital);
    RegisterWidget<HorizontalChartIndicator>(WidgetType::IndicatorHorizontalChart);
    RegisterWidget<DialIndicator>(WidgetType::IndicatorDial);
    RegisterWidget<BarIndicator>(WidgetType::IndicatorBar);
    RegisterWidget<SettingIndicator>(WidgetType::IndicatorSetting);

    RegisterWidget<SliderControl>(WidgetType::ControlSlider);
    RegisterWidget<ToggleControl>(WidgetType::ControlToggle);
    RegisterWidget<ButtonControl>(WidgetType::ControlButton);
}

} // namespace eerie_leap::views::widgets
