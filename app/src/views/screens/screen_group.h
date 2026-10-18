#pragma once

#include <cstdint>
#include <memory>
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
    std::shared_ptr<IScreen> GetScreen(uint32_t screen_id) const;
    bool IsEmpty() const;

    int EnsureRendered();
    bool IsRendered() const;

    void Activate();
    void Deactivate();

    bool IsActivated() const;
};

} // namespace eerie_leap::views::screens
