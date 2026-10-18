#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

#include <lvgl.h>

#include "views/renderable_base.h"
#include "views/screens/i_screen.h"
#include "views/screens/screen_group.h"

namespace eerie_leap::views {

using eerie_leap::views::screens::IScreen;
using eerie_leap::views::screens::ScreenGroup;

class MainView : public RenderableBase {
public:
    // Runs the given work somewhere other than the caller's thread.
    using RenderDispatcher = std::function<void(std::function<void()>)>;

private:
    std::unordered_map<uint32_t, std::shared_ptr<ScreenGroup>> screen_groups_;
    std::optional<uint32_t> active_screen_group_id_;
    RenderDispatcher render_dispatcher_;

    std::shared_ptr<ScreenGroup> GetOrCreateGroup(uint32_t screen_group_id);

    int RenderGroupOrRollBack(
        const std::shared_ptr<ScreenGroup>& screen_group,
        const std::shared_ptr<ScreenGroup>& previous_screen_group
    );

    int DoRender() override;
    int ApplyTheme(const ITheme& theme) override;

public:
    MainView();

    std::shared_ptr<Frame> GetContainer() const;

    // Building a group's LVGL tree costs tens of milliseconds and runs on a
    // large stack; without a dispatcher SetActiveGroup() renders inline on the
    // calling thread.
    void SetRenderDispatcher(RenderDispatcher dispatcher);

    // Screens must be parented to their group's container, so it is created up front.
    std::shared_ptr<Frame> GetGroupContainer(uint32_t screen_group_id);

    void AddScreen(std::shared_ptr<IScreen> screen);
    void PruneEmptyGroups();

    int SetActiveGroup(uint32_t screen_group_id);
    std::optional<uint32_t> GetActiveGroupId() const;

    std::vector<uint32_t> GetGroupIds() const;
    std::shared_ptr<IScreen> GetScreen(uint32_t screen_id) const;
};

} // namespace eerie_leap::views
