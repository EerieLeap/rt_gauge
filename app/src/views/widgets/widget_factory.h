#pragma once

#include <memory>
#include <functional>
#include <span>
#include <unordered_map>
#include <vector>

#include "domain/ui_domain/models/widget_type.h"

#include "views/widgets/i_widget.h"
#include "views/widgets/widget_context.h"
#include "views/utilitites/frame.h"

namespace eerie_leap::views::widgets {

using eerie_leap::domain::ui_domain::models::WidgetType;
using eerie_leap::domain::ui_domain::models::WidgetConfiguration;

using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::widgets::IWidget;

class WidgetFactory {
public:
    using WidgetCreator = std::function<std::unique_ptr<IWidget>(const uint32_t id, std::shared_ptr<Frame> container, const WidgetContext& context)>;
    using ChildValidator = std::function<void(const WidgetConfiguration&, std::span<const WidgetConfiguration* const>)>;

private:
    struct Registration {
        WidgetCreator create;
        ChildValidator validate_children;
    };
    std::unordered_map<WidgetType, Registration> registrations_;

    WidgetFactory();

    void RegisterTypes();

public:
    static WidgetFactory& GetInstance();

    template<typename T>
    void RegisterWidget(const WidgetType type) {
        RegisterWidget(type,
            [](uint32_t id, std::shared_ptr<Frame> parent, const WidgetContext& context) -> std::unique_ptr<IWidget> {
                return std::make_unique<T>(id, std::move(parent), context);
            }, T::ValidateChildren);
    }
    void RegisterWidget(const WidgetType type, WidgetCreator creator, ChildValidator validator = {});

    // Dispatches one widget-owned rule for the central validator; the supplied
    // children have already passed reference, ownership, type, and binding checks.
    void ValidateChildren(const WidgetConfiguration& owner, std::span<const WidgetConfiguration* const> children) const;

    std::unique_ptr<IWidget> CreateWidget(const WidgetType type, const uint32_t id, std::shared_ptr<Frame> parent, const WidgetContext& context);
    std::unique_ptr<IWidget> CreateWidget(std::shared_ptr<WidgetConfiguration> configuration, std::shared_ptr<Frame> parent, const WidgetContext& context);

    std::vector<WidgetType> GetAvailableTypes() const;
};

} // namespace eerie_leap::views::widgets
