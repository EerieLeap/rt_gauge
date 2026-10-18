#include <algorithm>
#include <exception>
#include <utility>

#include <zephyr/logging/log.h>

#include "domain/ui_domain/lvgl_lock.h"

#include "views/themes/default_theme.h"

#include "theme_manager.h"

namespace eerie_leap::views::themes {

using eerie_leap::domain::ui_domain::ScopedLvglLock;

LOG_MODULE_REGISTER(theme_manager_logger);

ThemeManager::ThemeManager() :
    current_theme_(std::make_shared<DefaultTheme>()) {
    k_mutex_init(&observers_lock_);
}

ThemeManager& ThemeManager::GetInstance() {
    static ThemeManager instance;
    return instance;
}

void ThemeManager::SetTheme(std::shared_ptr<ITheme> theme) {
    if(theme == nullptr || theme == current_theme_)
        return;

    // Observers repaint LVGL objects, so the switch must be serialized with the renderer.
    ScopedLvglLock lvgl_guard;

    current_theme_ = std::move(theme);
    NotifyObservers();
}

const ITheme& ThemeManager::GetCurrentTheme() const {
    return *current_theme_;
}

void ThemeManager::RegisterObserver(IThemeObserver* observer) {
    if(observer == nullptr)
        return;

    k_mutex_lock(&observers_lock_, K_FOREVER);
    observers_.push_back(observer);
    k_mutex_unlock(&observers_lock_);
}

void ThemeManager::UnregisterObserver(IThemeObserver* observer) {
    k_mutex_lock(&observers_lock_, K_FOREVER);

    observers_.erase(std::remove(observers_.begin(), observers_.end(), observer), observers_.end());

    k_mutex_unlock(&observers_lock_);
}

void ThemeManager::NotifyObservers() {
    // Snapshot: an observer may create or destroy renderables while handling the
    // change, and an erase would otherwise shift the vector and skip an observer.
    k_mutex_lock(&observers_lock_, K_FOREVER);
    const auto observers = observers_;
    k_mutex_unlock(&observers_lock_);

    for(auto* observer : observers) {
        try {
            observer->OnThemeChanged();
        } catch(const std::exception& e) {
            LOG_ERR("Theme observer failed: %s", e.what());
        } catch(...) {
            LOG_ERR("Theme observer failed.");
        }
    }
}

} // namespace eerie_leap::views::themes
