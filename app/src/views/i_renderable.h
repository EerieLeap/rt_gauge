#pragma once

#include <memory>

#include "views/themes/i_theme.h"
#include "views/themes/i_theme_observer.h"
#include "views/utilitites/frame.h"

namespace eerie_leap::views {

using eerie_leap::views::themes::ITheme;
using eerie_leap::views::themes::IThemeObserver;
using eerie_leap::views::utilitites::Frame;

class IRenderable : public IThemeObserver {
protected:
    virtual int DoRender() = 0;

public:
    virtual ~IRenderable() = default;

    virtual int Render() = 0;
    virtual int ApplyTheme(const ITheme& theme) = 0;
    virtual std::shared_ptr<Frame> GetContainer() const = 0;
    virtual bool IsReady() const = 0;
};

} // namespace eerie_leap::views
