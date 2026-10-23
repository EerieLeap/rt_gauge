#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include <zephyr/kernel.h>

#include "domain/ui_domain/models/navigation_intent.h"

namespace eerie_leap::domain::ui_domain::services {

using eerie_leap::domain::ui_domain::models::NavigationIntent;

class NavigationService {
private:
    static constexpr size_t HISTORY_DEPTH = 8;

    std::vector<uint32_t> screen_group_ids_;
    std::optional<uint32_t> active_screen_group_id_;

    std::array<uint32_t, HISTORY_DEPTH> history_{};
    size_t history_count_ = 0;

    std::optional<uint32_t> active_overlay_screen_id_;

    mutable k_mutex lock_{};

    void PushHistory(uint32_t screen_group_id);
    std::optional<uint32_t> PeekHistory() const;
    void PopHistory();
    int RequestGroup(uint32_t screen_group_id, bool record_history, std::optional<uint32_t>& screen_group_to_publish);
    static void PublishGroup(const std::optional<uint32_t>& screen_group_to_publish);
    std::optional<size_t> GetActiveIndex() const;

public:
    NavigationService();
    ~NavigationService() = default;

    // Copying would duplicate lock_, whose wait queue is tied to its address.
    NavigationService(const NavigationService&) = delete;
    NavigationService& operator=(const NavigationService&) = delete;

    // Forgets the groups, the history, the active group and an open overlay, for a UI that is about to be
    // rebuilt from another configuration. SetGroupIds() and SetActiveGroupId() follow.
    void Reset();

    void SetGroupIds(std::vector<uint32_t> screen_group_ids);
    void SetActiveGroupId(uint32_t screen_group_id);

    int GoToGroup(uint32_t screen_group_id);
    int Next();
    int Previous();
    int Back();
    int ShowOverlay(uint32_t screen_id);
    int CloseOverlay();

    int Handle(NavigationIntent intent, uint32_t argument = 0);

    std::optional<uint32_t> GetActiveGroupId() const;
    bool IsOverlayActive() const;
};

} // namespace eerie_leap::domain::ui_domain::services
