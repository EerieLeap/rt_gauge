#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "domain/ui_domain/models/ui_configuration.h"
#include "domain/ui_domain/preview/ui_preview_session.h"

namespace eerie_leap::domain::ui_preview_domain {

using eerie_leap::domain::ui_domain::models::UiConfiguration;
using eerie_leap::domain::ui_domain::preview::UiPreviewError;

/** @brief What SHOW names: the screen group of the app's screen, or an overlay's screen ID. */
struct UiPreviewTarget {
    bool is_overlay = false;
    uint32_t id = 0;
};

/** @brief What SHOW did: the group now shown, and the overlay over it, if one was asked for. */
struct UiPreviewShown {
    uint32_t group = 0;
    std::optional<uint32_t> overlay;
};

/**
 * @brief What a UI preview needs of the UI it previews on: UiController is the one implementation.
 *
 * Every call but ParseConfiguration() and the getters runs on the UI render work queue, which is
 * where the screens are rebuilt.
 */
class IUiPreviewHost {
public:
    virtual ~IUiPreviewHost() = default;

    /**
     * @brief Decodes and checks a staged configuration as an upload is checked, and stores nothing, from
     *        any work queue that has room to decode (the configuration work queue).
     * @param reason Receives why a configuration was refused.
     * @return The model, or null when it was refused.
     */
    virtual std::shared_ptr<UiConfiguration>
    ParseConfiguration(std::span<const uint8_t> cbor_data, std::span<char> reason) = 0;

    /** @brief Shows a configuration in place of the screens, as a store would, but stored nowhere. */
    virtual int ShowConfiguration(std::shared_ptr<UiConfiguration> configuration) = 0;

    /** @brief Shows what the unit stores again. */
    virtual int ShowStoredConfiguration() = 0;

    /** @brief Keeps the gestures from navigating away while pinned; the controls stay live. */
    virtual void SetNavigationPinned(bool is_pinned) = 0;

    /** @brief Asks navigation to show a group or an overlay of the configuration shown; NOT_FOUND if it has none. */
    virtual UiPreviewError ShowTarget(const UiPreviewTarget& target, UiPreviewShown& shown) = 0;

    /**
     * @brief Outlines a widget of the screens shown, following it. Neither @p screen nor @p widget clears the
     *        outline; a @p screen alone outlines the screen, and a @p widget alone the first widget of that ID on
     *        the screens of the group shown.
     * @return NOT_FOUND if there is no such screen or widget.
     */
    virtual UiPreviewError Highlight(std::optional<uint32_t> screen, std::optional<uint32_t> widget) = 0;

    /** @brief The CRC-32 of the stored UI configuration. */
    [[nodiscard]] virtual uint32_t GetStoredCrc() const = 0;
    /** @brief The group shown, once navigation knows it. */
    [[nodiscard]] virtual std::optional<uint32_t> GetActiveGroupId() const = 0;
};

} // namespace eerie_leap::domain::ui_preview_domain
