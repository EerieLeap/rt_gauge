#include "views/widgets/basic/icons/dot_icon/dot_icon.h"
#include "views/widgets/basic/icons/label_icon/label_icon.h"
#include "views/widgets/basic/icons/image_icon/image_icon.h"
#include "views/widgets/basic/icons/shape_icon/shape_icon.h"

#include "icon_factory.h"

namespace eerie_leap::views::widgets {

using namespace eerie_leap::views::widgets::basic::icons;

IconFactory::IconFactory() {
    RegisterTypes();
}

IconFactory& IconFactory::GetInstance() {
    static IconFactory instance;
    return instance;
}

template<typename T>
void IconFactory::Register(const IconType type) {
    creators_[type] = [](std::shared_ptr<Frame> parent) -> std::unique_ptr<IIcon> {
        return std::make_unique<T>(parent);
    };
}

void IconFactory::Register(const IconType type, IconCreator creator) {
    creators_[type] = std::move(creator);
}

std::unique_ptr<IIcon> IconFactory::Create(const IconType type, std::shared_ptr<WidgetPropertyStore> properties, std::shared_ptr<Frame> parent) {
    auto it = creators_.find(type);
    if (it == creators_.end())
        throw std::runtime_error("Unknown widget type");

    auto icon = it->second(parent);
    icon->Configure(std::move(properties));

    return icon;
}

std::vector<IconType> IconFactory::GetAvailableTypes() const {
    std::vector<IconType> types;
    types.reserve(creators_.size());

    for (const auto& [type, creator] : creators_)
        types.push_back(type);

    return types;
}

void IconFactory::RegisterTypes() {
    Register<DotIcon>(IconType::Dot);
    Register<LabelIcon>(IconType::Label);
    Register<ImageIcon>(IconType::Image);

    for(auto type : { IconType::Rectangle, IconType::TriangleIsosceles, IconType::TriangleRight, IconType::Oval, IconType::Line }) {
        Register(type, [type](std::shared_ptr<Frame> parent) {
            return std::make_unique<ShapeIcon>(std::move(parent), type);
        });
    }
}

} // namespace eerie_leap::views::widgets
