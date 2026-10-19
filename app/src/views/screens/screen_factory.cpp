#include <stdexcept>
#include <utility>

#include "views/screens/screen.h"

#include "screen_factory.h"

namespace eerie_leap::views::screens {

ScreenFactory::ScreenFactory() {
    RegisterTypes();
}

ScreenFactory& ScreenFactory::GetInstance() {
    static ScreenFactory instance;
    return instance;
}

void ScreenFactory::RegisterScreen(const ScreenType type, ScreenCreator creator) {
    creators_[type] = std::move(creator);
}

std::shared_ptr<IScreen> ScreenFactory::CreateScreen(
    const ScreenType type,
    const uint32_t id,
    std::shared_ptr<Frame> parent,
    const WidgetContext& context
) {
    const auto it = creators_.find(type);
    if(it == creators_.end())
        throw std::runtime_error("Unknown screen type");

    return it->second(id, std::move(parent), context);
}

std::shared_ptr<IScreen> ScreenFactory::CreateScreen(
    std::shared_ptr<ScreenConfiguration> configuration,
    std::shared_ptr<Frame> parent,
    const WidgetContext& context
) {
    auto screen = CreateScreen(configuration->type, configuration->id, std::move(parent), context);
    screen->Configure(std::move(configuration));

    return screen;
}

std::vector<ScreenType> ScreenFactory::GetAvailableTypes() const {
    std::vector<ScreenType> types;
    types.reserve(creators_.size());

    for(const auto& [type, creator] : creators_)
        types.push_back(type);

    return types;
}

// Every screen type is a widget list today; hand-written IScreen subclasses can
// be registered here later without touching UiController.
void ScreenFactory::RegisterTypes() {
    RegisterScreen<Screen>(ScreenType::System);
    RegisterScreen<Screen>(ScreenType::Gauge);
    RegisterScreen<Screen>(ScreenType::Settings);
    RegisterScreen<Screen>(ScreenType::Popup);
}

} // namespace eerie_leap::views::screens
