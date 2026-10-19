#include <algorithm>
#include <cerrno>
#include <utility>

#include <zephyr/logging/log.h>

#include "utilities/reflection/caller_name.h"

#include "domain/ui_domain/event_bus/navigation_event_channel.h"

#include "navigation_service.h"

namespace eerie_leap::domain::ui_domain::services {

using namespace eerie_leap::domain::ui_domain::event_bus;

using eerie_leap::domain::ui_domain::models::NavigationAction;
using eerie_leap::utilities::reflection::GetCallerName;

LOG_MODULE_REGISTER(navigation_service_logger);

namespace {

void PublishNavigation(NavigationAction action, NavigationPayloadType target_type, uint32_t target_id) {
    static constexpr auto CALLER = GetCallerName();

    NavigationEventChannel::GetInstance().PublishAsync(
        { .source_id = CALLER.hash,
            .type = NavigationEventType::Changed,
            .payload = {
                { NavigationPayloadType::Action, static_cast<uint32_t>(action) }, { target_type, target_id } } }
    );
}

} // namespace

NavigationService::NavigationService() {
    k_mutex_init(&lock_);
}

void NavigationService::SetGroupIds(std::vector<uint32_t> screen_group_ids) {
    k_mutex_lock(&lock_, K_FOREVER);

    screen_group_ids_ = std::move(screen_group_ids);
    std::ranges::sort(screen_group_ids_);

    k_mutex_unlock(&lock_);
}

void NavigationService::SetActiveGroupId(uint32_t screen_group_id) {
    k_mutex_lock(&lock_, K_FOREVER);

    active_screen_group_id_ = screen_group_id;

    k_mutex_unlock(&lock_);
}

void NavigationService::PushHistory(uint32_t screen_group_id) {
    if(history_count_ == HISTORY_DEPTH) {
        std::ranges::rotate(history_, history_.begin() + 1);
        --history_count_;
    }

    history_[history_count_++] = screen_group_id;
}

std::optional<uint32_t> NavigationService::PeekHistory() const {
    if(history_count_ == 0)
        return std::nullopt;

    return history_[history_count_ - 1];
}

void NavigationService::PopHistory() {
    if(history_count_ > 0)
        --history_count_;
}

std::optional<size_t> NavigationService::GetActiveIndex() const {
    if(!active_screen_group_id_.has_value())
        return std::nullopt;

    const auto it = std::ranges::find(screen_group_ids_, *active_screen_group_id_);
    if(it == screen_group_ids_.end())
        return std::nullopt;

    return static_cast<size_t>(std::distance(screen_group_ids_.begin(), it));
}

int NavigationService::RequestGroup(
    uint32_t screen_group_id,
    bool record_history,
    std::optional<uint32_t>& screen_group_to_publish
) {
    if(active_overlay_screen_id_.has_value())
        return -EBUSY;

    if(std::ranges::find(screen_group_ids_, screen_group_id) == screen_group_ids_.end()) {
        LOG_WRN("Screen group %u is not available.", screen_group_id);
        return -ENOENT;
    }

    if(active_screen_group_id_ == screen_group_id)
        return 0;

    if(record_history && active_screen_group_id_.has_value())
        PushHistory(*active_screen_group_id_);

    // Advance intent now; the view reconciles it via SetActiveGroupId once applied.
    // Without this, requests issued back to back would all target the same group.
    active_screen_group_id_ = screen_group_id;
    screen_group_to_publish = screen_group_id;

    return 0;
}

void NavigationService::PublishGroup(const std::optional<uint32_t>& screen_group_to_publish) {
    if(screen_group_to_publish.has_value())
        PublishNavigation(NavigationAction::ShowGroup, NavigationPayloadType::TargetGroupId, *screen_group_to_publish);
}

int NavigationService::GoToGroup(uint32_t screen_group_id) {
    std::optional<uint32_t> screen_group_to_publish;

    k_mutex_lock(&lock_, K_FOREVER);
    const int res = RequestGroup(screen_group_id, true, screen_group_to_publish);
    k_mutex_unlock(&lock_);

    PublishGroup(screen_group_to_publish);

    return res;
}

int NavigationService::Next() {
    std::optional<uint32_t> screen_group_to_publish;

    k_mutex_lock(&lock_, K_FOREVER);

    int res = -ENOENT;
    if(auto index = GetActiveIndex())
        res = RequestGroup(screen_group_ids_[(*index + 1) % screen_group_ids_.size()], true, screen_group_to_publish);

    k_mutex_unlock(&lock_);

    PublishGroup(screen_group_to_publish);

    return res;
}

int NavigationService::Previous() {
    std::optional<uint32_t> screen_group_to_publish;

    k_mutex_lock(&lock_, K_FOREVER);

    int res = -ENOENT;
    if(auto index = GetActiveIndex())
        res = RequestGroup(
            screen_group_ids_[(*index + screen_group_ids_.size() - 1) % screen_group_ids_.size()], true,
            screen_group_to_publish
        );

    k_mutex_unlock(&lock_);

    PublishGroup(screen_group_to_publish);

    return res;
}

int NavigationService::Back() {
    std::optional<uint32_t> screen_group_to_publish;

    k_mutex_lock(&lock_, K_FOREVER);

    int res = -ENOENT;
    if(auto screen_group_id = PeekHistory()) {
        res = RequestGroup(*screen_group_id, false, screen_group_to_publish);

        // Only consume the entry once the request was accepted.
        if(res == 0)
            PopHistory();
    }

    k_mutex_unlock(&lock_);

    PublishGroup(screen_group_to_publish);

    return res;
}

int NavigationService::ShowOverlay(uint32_t screen_id) {
    k_mutex_lock(&lock_, K_FOREVER);

    // Only one overlay is tracked here, while OverlayHost can stack several. Letting
    // a second one through would make a single CloseOverlay unblock group navigation
    // with an overlay still on screen.
    const bool is_busy = active_overlay_screen_id_.has_value();
    if(!is_busy)
        active_overlay_screen_id_ = screen_id;

    k_mutex_unlock(&lock_);

    if(is_busy)
        return -EBUSY;

    PublishNavigation(NavigationAction::ShowOverlay, NavigationPayloadType::TargetScreenId, screen_id);

    return 0;
}

int NavigationService::CloseOverlay() {
    k_mutex_lock(&lock_, K_FOREVER);
    auto closed_screen_id = active_overlay_screen_id_;
    active_overlay_screen_id_.reset();
    k_mutex_unlock(&lock_);

    if(!closed_screen_id.has_value())
        return -ENOENT;

    PublishNavigation(NavigationAction::CloseOverlay, NavigationPayloadType::TargetScreenId, *closed_screen_id);

    return 0;
}

int NavigationService::Handle(NavigationIntent intent, uint32_t argument) {
    switch(intent) {
        case NavigationIntent::NextGroup:
            return Next();
        case NavigationIntent::PreviousGroup:
            return Previous();
        case NavigationIntent::GoToGroup:
            return GoToGroup(argument);
        case NavigationIntent::Back:
            return Back();
        case NavigationIntent::ShowOverlay:
            return ShowOverlay(argument);
        case NavigationIntent::CloseOverlay:
            return CloseOverlay();
        case NavigationIntent::None:
        default:
            return -EINVAL;
    }
}

std::optional<uint32_t> NavigationService::GetActiveGroupId() const {
    k_mutex_lock(&lock_, K_FOREVER);
    auto active_screen_group_id = active_screen_group_id_;
    k_mutex_unlock(&lock_);

    return active_screen_group_id;
}

bool NavigationService::IsOverlayActive() const {
    k_mutex_lock(&lock_, K_FOREVER);
    const bool is_active = active_overlay_screen_id_.has_value();
    k_mutex_unlock(&lock_);

    return is_active;
}

} // namespace eerie_leap::domain::ui_domain::services
