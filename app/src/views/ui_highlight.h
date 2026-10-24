#pragma once

#include <cstdint>
#include <functional>
#include <optional>

#include <lvgl.h>

namespace eerie_leap::views {

/**
 * @brief An outline over a widget of the screens, for an editor that shows which widget it is working on.
 *
 * The outline is an object of its own on lv_layer_top(), above the screens and the overlays, that takes no input
 * (it is not clickable, so a touch goes to what is under it) and follows the widget's absolute coordinates: a timer
 * on the renderer thread resolves the widget again, by the IDs the outline was set with, every tick. It therefore
 * follows a widget that moves or is resized, hides while the widget is gone (the screens are rebuilt), and shows
 * again when a widget with those IDs is back.
 *
 * Every method takes the LVGL lock.
 */
class UiHighlight {
public:
    /** @brief Gives the object of the widget (or screen, with no widget) to outline, or null; under the LVGL lock. */
    using Resolver = std::function<lv_obj_t*(std::optional<uint32_t> screen, std::optional<uint32_t> widget)>;

    static constexpr uint32_t FOLLOW_PERIOD_MS = 33;
    static constexpr int32_t BORDER_PX = 2;

private:
    Resolver resolver_;
    lv_obj_t* outline_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    std::optional<uint32_t> screen_;
    std::optional<uint32_t> widget_;
    bool is_set_ = false;

    static void FollowCb(lv_timer_t* timer);
    void Follow();
    void Hide();

public:
    explicit UiHighlight(Resolver resolver);
    ~UiHighlight();

    UiHighlight(const UiHighlight&) = delete;
    UiHighlight& operator=(const UiHighlight&) = delete;
    UiHighlight(UiHighlight&&) = delete;
    UiHighlight& operator=(UiHighlight&&) = delete;

    /** @brief Outlines the widget (or the screen) from now on; replaces an earlier outline. */
    void Set(std::optional<uint32_t> screen, std::optional<uint32_t> widget);
    void Clear();

    [[nodiscard]] bool IsSet() const;
    /** @brief Whether the outline is on screen: set, and its target found. */
    [[nodiscard]] bool IsVisible() const;
    /** @brief The outline's object; null until the first Set(). */
    [[nodiscard]] lv_obj_t* GetObject() const;
};

} // namespace eerie_leap::views
