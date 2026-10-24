#pragma once

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "views/renderable_base.h"
#include "views/screens/i_screen.h"

namespace eerie_leap::views::screens {

// A set of screens rendered on top of each other and shown/hidden as a unit.
class ScreenGroup : public RenderableBase {
private:
    uint32_t screen_group_id_;
    bool is_rendered_ = false;
    bool is_activated_ = false;

    // Activation is requested synchronously but applied once the group is
    // rendered, which may happen later on another thread.
    bool is_activation_requested_ = false;
    std::vector<std::shared_ptr<IScreen>> screens_;

    void ApplyScreenOrder();
    void ApplyActivation();

    int DoRender() override;
    int ApplyTheme(const ITheme& theme) override;

public:
    ScreenGroup(uint32_t screen_group_id, const std::shared_ptr<Frame>& parent);

    uint32_t GetGroupId() const;

    void AddScreen(std::shared_ptr<IScreen> screen);
    // Puts @p screen in the place of the screen of its ID, or adds it. A group that is rendered renders it, one
    // that is active activates it, and the screens end in drawing order. The screen it replaces goes after.
    void PutScreen(std::shared_ptr<IScreen> screen);
    // Takes a screen out, with everything it built. False if the group has none of that ID.
    bool RemoveScreen(uint32_t screen_id);
    // Puts the screens in the order of @p screen_ids, bottom first, in the vector and in LVGL; a screen not listed
    // follows the listed ones in the order it had.
    void OrderScreens(std::span<const uint32_t> screen_ids);
    std::shared_ptr<IScreen> GetScreen(uint32_t screen_id) const;
    std::vector<std::shared_ptr<IScreen>> GetScreens() const;
    bool IsEmpty() const;

    int EnsureRendered();
    bool IsRendered() const;

    void Activate();
    void Deactivate();

    bool IsActivated() const;
};

} // namespace eerie_leap::views::screens
