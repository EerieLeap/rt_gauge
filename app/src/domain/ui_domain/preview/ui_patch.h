#pragma once

#include <cstdint>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <vector>

#include "domain/ui_domain/models/ui_configuration.h"

namespace eerie_leap::domain::ui_domain::preview {

using eerie_leap::domain::ui_domain::models::ScreenConfiguration;
using eerie_leap::domain::ui_domain::models::UiConfiguration;
using eerie_leap::domain::ui_domain::models::WidgetConfiguration;

/**
 * @brief How much of the screens a change of one widget, or of one screen, takes to show.
 *
 * Plan `gauge-ui-editor.md`, Appendix B, "What applies in place". In the firmware's words a "screen" is the
 * editor's group: a stack of widgets that belongs to a screen group, the editor's screen.
 */
enum class WidgetChange : uint8_t {
    /// The widget object stays and takes the new values through its inbound property path.
    InPlace,
    /// The widget, and the widgets it owns, are built again.
    RebuildWidget,
    /// The whole screen is built again.
    RebuildGroup,
};

/**
 * @brief How a root widget's configuration changed from @p old_widget to @p new_widget, which have one ID.
 *
 * In place: the properties a widget applies to its objects as they change (visibility, opacity, activity, the
 * colors, the range and the value, the precision, the unit, a setting indicator's or button's label, the
 * angles of an arc, the anchor, the animation, a shape icon's offsets, sizes, stroke, corner, fill and
 * direction), a root widget's position and size, and its z-index among its siblings. Everything else rebuilds
 * the widget: the type, the bindings, the icon type, a file and an image's size, a label icon's label, a chart's
 * points and type, the structure, and any property this does not know to apply in place. A widget that owns
 * children is classified by its own record; what changed in a child rebuilds its owner, which the caller decides
 * (PlanPatch()). Never RebuildGroup: a group is rebuilt for what the widget does not say, see ClassifyScreenChange().
 */
[[nodiscard]] WidgetChange ClassifyWidgetChange(const WidgetConfiguration& old_widget, const WidgetConfiguration& new_widget);

/**
 * @brief What the screen's own record changed, leaving its widgets aside: InPlace, or RebuildGroup for a change
 *        of the grid, the type, the visibility, the z-index, the screen group or the overlay flag.
 */
[[nodiscard]] WidgetChange ClassifyScreenChange(const ScreenConfiguration& old_screen, const ScreenConfiguration& new_screen);

/// Whether two records are the same widget in every field.
[[nodiscard]] bool IsSameWidget(const WidgetConfiguration& a, const WidgetConfiguration& b);

/// What a PATCH operation does (the wire's operation numbers).
enum class PatchOpKind : uint8_t {
    PutWidget = 0,    ///< Adds the widget to the screen, or replaces the one of its ID.
    RemoveWidget = 1, ///< Removes the widget and everything it owns.
    PutScreen = 2,    ///< Adds the screen, or replaces the one of its ID.
    RemoveScreen = 3,
};

struct PatchOp {
    PatchOpKind kind = PatchOpKind::PutWidget;
    /// The screen the operation is on (the screen's own ID for PutScreen).
    uint32_t screen_id = 0;
    /// The widget removed by RemoveWidget; a PutWidget names its widget in the record.
    uint32_t widget_id = 0;
    std::shared_ptr<WidgetConfiguration> widget;
    std::shared_ptr<ScreenConfiguration> screen;
};

/// An operation names a screen or a widget the configuration it patches does not have.
class PatchBaseMismatch : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/**
 * @brief The operations applied in order onto a copy of @p base: all of them or none.
 *
 * Shares every screen and widget the operations leave alone with @p base, so only the touched screens are
 * copied. Does not validate the result.
 *
 * @throws PatchBaseMismatch for an operation on a screen or widget @p base lacks at that point.
 */
[[nodiscard]] std::shared_ptr<UiConfiguration>
ApplyPatchOps(const UiConfiguration& base, std::span<const PatchOp> ops, std::pmr::memory_resource* mr);

/// What a patch did to the screens, per operation.
enum class PatchEffect : uint8_t {
    InPlace = 0,
    WidgetRebuilt = 1,
    GroupRebuilt = 2,
    NotShown = 3,
};

enum class WidgetAction : uint8_t {
    Update,  ///< In place, onto the widget that is there.
    Replace, ///< Built again in the place of the one that is there.
    Add,
    Remove,
};

/// One root widget of a screen.
struct WidgetPatch {
    uint32_t widget_id = 0;
    WidgetAction action = WidgetAction::Update;
    /// The record to show (null for Remove).
    std::shared_ptr<WidgetConfiguration> widget;
};

enum class ScreenAction : uint8_t {
    Widgets, ///< The screen stays, and so do its widgets that are not in `widgets`.
    Rebuild, ///< The screen is built again from `new_screen`.
    Add,
    Remove,
};

struct ScreenPatch {
    uint32_t screen_id = 0;
    ScreenAction action = ScreenAction::Widgets;
    std::shared_ptr<ScreenConfiguration> old_screen;
    std::shared_ptr<ScreenConfiguration> new_screen;
    /// Removes first, then the others; for ScreenAction::Widgets only.
    std::vector<WidgetPatch> widgets;
};

struct PatchPlan {
    std::vector<ScreenPatch> screens;
    /// Per operation, as if every screen were shown; the view adjusts those on an overlay.
    std::vector<PatchEffect> effects;
    /// Per operation, the screen it was on.
    std::vector<uint32_t> op_screen_ids;
};

/**
 * @brief What it takes to go from @p base to @p patched, which @p ops made of it and which passed validation.
 *
 * A screen whose record changed (ClassifyScreenChange()) is rebuilt. For one whose record did not, each root
 * widget is compared with the one of its ID: a root whose subtree is the same set of widgets in the same
 * order is updated in place when only its own record changed in place (ClassifyWidgetChange()); any other
 * change of it, or of a widget it owns, replaces it; roots that are new are added, those that are gone removed.
 */
[[nodiscard]] PatchPlan PlanPatch(const UiConfiguration& base, const UiConfiguration& patched, std::span<const PatchOp> ops);

} // namespace eerie_leap::domain::ui_domain::preview
