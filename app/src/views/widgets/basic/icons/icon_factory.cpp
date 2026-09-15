#include "views/widgets/basic/icons/dot_icon/dot_icon.h"
#include "views/widgets/basic/icons/label_icon/label_icon.h"
#include "views/widgets/basic/icons/image_icon/image_icon.h"
#include "views/widgets/basic/icons/shape_icon/rectangle_icon/rectangle_icon.h"
#include "views/widgets/basic/icons/shape_icon/isosceles_triangle_icon/isosceles_triangle_icon.h"
#include "views/widgets/basic/icons/shape_icon/right_triangle_icon/right_triangle_icon.h"
#include "views/widgets/basic/icons/shape_icon/oval_icon/oval_icon.h"
#include "views/widgets/basic/icons/shape_icon/line_icon/line_icon.h"

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
    Register(type, [](std::shared_ptr<Frame> parent) -> std::unique_ptr<IIcon> {
        return std::make_unique<T>(std::move(parent));
    }, T::RegisterProperties);
}

void IconFactory::Register(const IconType type, IconCreator creator, PropertyRegistrar registrar) {
    registrations_[type] = { std::move(creator), std::move(registrar) };
}

void IconFactory::RegisterProperties(IconType type, WidgetPropertyStore& store) const {
    auto it = registrations_.find(type);
    if(it != registrations_.end() && it->second.register_properties)
        it->second.register_properties(store);
}

std::unique_ptr<IIcon> IconFactory::Create(const IconType type, std::shared_ptr<WidgetPropertyStore> properties, std::shared_ptr<Frame> parent) {
    auto it = registrations_.find(type);
    if (it == registrations_.end())
        throw std::runtime_error("Unknown widget type");

    auto icon = it->second.create(parent);
    icon->Configure(std::move(properties));

    return icon;
}

std::vector<IconType> IconFactory::GetAvailableTypes() const {
    std::vector<IconType> types;
    types.reserve(registrations_.size());

    for (const auto& [type, registration] : registrations_)
        types.push_back(type);

    return types;
}

void IconFactory::RegisterTypes() {
    Register<DotIcon>(IconType::Dot);
    Register<LabelIcon>(IconType::Label);
    Register<ImageIcon>(IconType::Image);

    Register<RectangleIcon>(IconType::Rectangle);
    Register<IsoscelesTriangleIcon>(IconType::TriangleIsosceles);
    Register<RightTriangleIcon>(IconType::TriangleRight);
    Register<OvalIcon>(IconType::Oval);
    Register<LineIcon>(IconType::Line);
}

} // namespace eerie_leap::views::widgets
