#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
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

    std::shared_ptr<Frame> GetContainer() const override;

    // Building a group's LVGL tree costs tens of milliseconds and runs on a
    // large stack; without a dispatcher SetActiveGroup() renders inline on the
    // calling thread.
    void SetRenderDispatcher(RenderDispatcher dispatcher);

    // Screens must be parented to their group's container, so it is created up front.
    std::shared_ptr<Frame> GetGroupContainer(uint32_t screen_group_id);

    void AddScreen(std::shared_ptr<IScreen> screen);
    // Puts a screen in the place of the screen of its ID in its group, or adds it to the group, which must be
    // there; false if it is not. The screen it replaces goes with its objects.
    bool PutScreen(std::shared_ptr<IScreen> screen);
    // Takes a screen out of whichever group has it. False if none does, or if it is the last of its group:
    // a group with no screens is not a group, so the caller builds the view again.
    bool RemoveScreen(uint32_t screen_id);
    // Puts the screens of a group in the drawing order of @p screen_ids, bottom first.
    void OrderScreens(uint32_t screen_group_id, std::span<const uint32_t> screen_ids);
    void PruneEmptyGroups();

    // Drops every screen group with everything it built, and the active one, so the view can be configured
    // again. Takes the LVGL lock; the render dispatcher stays.
    void Clear();

    // @param render_inline Renders a group that was not rendered yet on the calling thread, not on the
    //        render dispatcher, so it is on screen when this returns and no frame shows without it.
    int SetActiveGroup(uint32_t screen_group_id, bool render_inline = false);
    std::optional<uint32_t> GetActiveGroupId() const;

    std::vector<uint32_t> GetGroupIds() const;
    std::shared_ptr<IScreen> GetScreen(uint32_t screen_id) const;
    // The screens of a group, in drawing order; none for a group that is not there.
    std::vector<std::shared_ptr<IScreen>> GetGroupScreens(uint32_t screen_group_id) const;
};

} // namespace eerie_leap::views
