#pragma once

#include <memory>
#include <functional>
#include <unordered_map>
#include <vector>

#include "views/widgets/basic/icons/i_icon.h"

namespace eerie_leap::views::widgets::basic::icons {

class IconFactory {
public:
    using IconCreator = std::function<std::unique_ptr<IIcon>(std::shared_ptr<Frame> container)>;
    using PropertyRegistrar = std::function<void(WidgetPropertyStore& store)>;

private:
    struct Registration {
        IconCreator create;
        PropertyRegistrar register_properties;
    };
    std::unordered_map<IconType, Registration> registrations_;

    IconFactory();

    void RegisterTypes();

public:
    static IconFactory& GetInstance();

    template<typename T>
    void Register(const IconType type);
    void Register(const IconType type, IconCreator creator, PropertyRegistrar registrar = {});

    void RegisterProperties(IconType type, WidgetPropertyStore& store) const;

    std::unique_ptr<IIcon> Create(const IconType type, std::shared_ptr<WidgetPropertyStore> properties, std::shared_ptr<Frame> parent);

    std::vector<IconType> GetAvailableTypes() const;
};

} // namespace eerie_leap::views::widgets::basic::icons
