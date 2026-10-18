#pragma once

#include <memory>
#include <vector>

#include <zephyr/kernel.h>

#include "views/themes/i_theme.h"
#include "views/themes/i_theme_observer.h"

namespace eerie_leap::views::themes {

class ThemeManager {
private:
    std::shared_ptr<ITheme> current_theme_;
    std::vector<IThemeObserver*> observers_;

    // Renderables register/unregister from their ctor/dtor on any thread while
    // the renderer or event bus thread can be iterating observers_.
    k_mutex observers_lock_{};

    ThemeManager();
    ~ThemeManager() = default;

    ThemeManager(const ThemeManager&) = delete;
    ThemeManager& operator=(const ThemeManager&) = delete;

    void NotifyObservers();

public:
    static ThemeManager& GetInstance();

    void SetTheme(std::shared_ptr<ITheme> theme);
    const ITheme& GetCurrentTheme() const;

    void RegisterObserver(IThemeObserver* observer);
    void UnregisterObserver(IThemeObserver* observer);
};

} // namespace eerie_leap::views::themes
