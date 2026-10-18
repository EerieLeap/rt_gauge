#pragma once

#include <memory>
#include <zephyr/kernel.h>

#include "views/i_renderable.h"
#include "views/themes/theme_manager.h"

namespace eerie_leap::views {

using eerie_leap::views::themes::ThemeManager;

class RenderableBase : public virtual IRenderable {
protected:
    std::shared_ptr<Frame> container_;
    bool is_ready_ = false;

    void OnThemeChanged() override {
        if(container_ == nullptr || container_->GetObject() == nullptr)
            return;

        if(!is_ready_)
            return;

        ApplyTheme(ThemeManager::GetInstance().GetCurrentTheme());
        container_->Invalidate();
    }

public:
    RenderableBase() {
        ThemeManager::GetInstance().RegisterObserver(this);
    }

    ~RenderableBase() override {
        ThemeManager::GetInstance().UnregisterObserver(this);
        is_ready_ = false;
    }

    std::shared_ptr<Frame> GetContainer() const override {
        return container_;
    }

    int Render() override {
        int res = DoRender();
        if(res != 0)
            return res;

        res = ApplyTheme(ThemeManager::GetInstance().GetCurrentTheme());
        if(res != 0)
            return res;

        k_msleep(10); // Allow LVGL to render before invalidation
        container_->Invalidate();

        is_ready_ = res == 0;

        return res;
    }

    bool IsReady() const override {
        return is_ready_;
    }
};

} // namespace eerie_leap::views
