#pragma once

#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

#include "domain/ui_domain/models/screen_configuration.h"
#include "domain/ui_domain/models/screen_type.h"

#include "views/screens/i_screen.h"
#include "views/utilitites/frame.h"
#include "views/widgets/widget_context.h"

namespace eerie_leap::views::screens {

using eerie_leap::domain::ui_domain::models::ScreenConfiguration;
using eerie_leap::domain::ui_domain::models::ScreenType;
using eerie_leap::views::utilitites::Frame;
using eerie_leap::views::widgets::WidgetContext;

class ScreenFactory {
public:
    using ScreenCreator = std::function<
        std::shared_ptr<IScreen>(const uint32_t id, std::shared_ptr<Frame> parent, const WidgetContext& context)>;

private:
    std::unordered_map<ScreenType, ScreenCreator> creators_;

    ScreenFactory();

    void RegisterTypes();

public:
    static ScreenFactory& GetInstance();

    template<typename T>
    void RegisterScreen(const ScreenType type) {
        creators_[type] = [](const uint32_t id, std::shared_ptr<Frame> parent,
                              const WidgetContext& context) -> std::shared_ptr<IScreen> {
            return std::make_shared<T>(id, std::move(parent), context);
        };
    }

    void RegisterScreen(const ScreenType type, ScreenCreator creator);

    std::shared_ptr<IScreen>
    CreateScreen(const ScreenType type, const uint32_t id, std::shared_ptr<Frame> parent, const WidgetContext& context);
    std::shared_ptr<IScreen> CreateScreen(
        std::shared_ptr<ScreenConfiguration> configuration,
        std::shared_ptr<Frame> parent,
        const WidgetContext& context
    );

    std::vector<ScreenType> GetAvailableTypes() const;
};

} // namespace eerie_leap::views::screens
