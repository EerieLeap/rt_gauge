#include <algorithm>
#include <array>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include "eerie_memory.hpp"

#include "domain/ui_domain/models/icon_type.h"
#include "domain/ui_domain/models/widget_composition.h"
#include "domain/ui_domain/models/widget_property.h"
#include "domain/ui_domain/models/widget_type.h"
#include "domain/ui_domain/utilities/widget_property_validator.h"

#include "ui_patch.h"

namespace eerie_leap::domain::ui_domain::preview {

using namespace eerie_leap::domain::ui_domain::models;

using eerie_leap::domain::ui_domain::utilities::WidgetPropertyValidator;
using eerie_leap::utilities::type::ConfigValue;

namespace {

bool IsShapeIcon(IconType type) {
    switch(type) {
        case IconType::Rectangle:
        case IconType::TriangleIsosceles:
        case IconType::TriangleRight:
        case IconType::Oval:
        case IconType::Line:
            return true;

        default:
            return false;
    }
}

IconType IconTypeOf(const WidgetConfiguration& widget) {
    const auto it = widget.properties.find(WidgetPropertyType::ICON_TYPE);
    if(it == widget.properties.end())
        return IconType::None;

    if(const auto* number = std::get_if<int>(&it->second))
        return static_cast<IconType>(*number);

    if(const auto* real = std::get_if<double>(&it->second))
        return static_cast<IconType>(static_cast<int>(*real));

    return IconType::None;
}

// What every widget applies as it changes: management, color, anchor and animation.
bool IsCommonInPlaceProperty(WidgetPropertyType type) {
    // Not IS_ACTIVE, which stops a widget's processing and tracking: what that leaves on the objects (a segment arc
    // that is deactivated is not the one that was built so) is the widget's to say, and it is built again.
    return type == WidgetPropertyType::IS_VISIBLE
        || type == WidgetPropertyType::OPACITY
        || WidgetPropertyValidator::IsColorProperty(type)
        || WidgetPropertyValidator::IsAnchorProperty(type)
        || WidgetPropertyValidator::IsAnimationProperty(type);
}

// Whether a widget of this type applies a change of this property to its objects as it comes, so that the widget
// looks as one built from the new record does. The table is short on purpose: what a widget reads once, as it is
// rendered (a range, an angle, a precision, a label, the points of a chart, the segments of an arc), is a rebuild,
// and so is a value, whose live display belongs to the animation and the history of the widget that shows it.
// Whatever is not listed is a rebuild, which is always right and sometimes slow.
bool AppliesInPlace(const WidgetConfiguration& widget, WidgetPropertyType type) {
    if(IsCommonInPlaceProperty(type))
        return true;

    // A shape icon's size, stroke, corner, fill, direction and offsets are applied by the icon widget as they change.
    // An arc icon places its icon on a circle as it renders; the others have none of these.
    if(widget.type != WidgetType::BasicIcon || !IsShapeIcon(IconTypeOf(widget)))
        return false;

    return type == WidgetPropertyType::POSITION_X
        || type == WidgetPropertyType::POSITION_Y
        || type == WidgetPropertyType::WIDTH_PX
        || type == WidgetPropertyType::HEIGHT_PX
        || type == WidgetPropertyType::STROKE_PX
        || type == WidgetPropertyType::CORNER_RAD_PX
        || type == WidgetPropertyType::FILL_MODE
        || type == WidgetPropertyType::DIRECTION;
}

bool IsSameBinding(const PropertyBinding& a, const PropertyBinding& b) {
    return a.target == b.target
        && a.direction == b.direction
        && a.outbound_event_type == b.outbound_event_type
        && a.event.channel == b.event.channel
        && a.event.event_type == b.event.event_type
        && a.event.payload_key == b.event.payload_key
        && a.event.selector_key == b.event.selector_key
        && a.event.selector_value == b.event.selector_value;
}

bool IsSameProperties(
    const std::pmr::unordered_map<WidgetPropertyType, ConfigValue>& a,
    const std::pmr::unordered_map<WidgetPropertyType, ConfigValue>& b
) {
    if(a.size() != b.size())
        return false;

    return std::ranges::all_of(a, [&b](const auto& entry) {
        const auto it = b.find(entry.first);

        return it != b.end() && it->second == entry.second;
    });
}

bool IsSameBindings(const std::pmr::vector<PropertyBinding>& a, const std::pmr::vector<PropertyBinding>& b) {
    return std::ranges::equal(a, b, IsSameBinding);
}

bool IsSameGrid(const GridSettings& a, const GridSettings& b) {
    return a.snap_enabled == b.snap_enabled
        && a.width == b.width
        && a.height == b.height
        && a.spacing_px == b.spacing_px;
}

std::shared_ptr<ScreenConfiguration> CloneScreen(const ScreenConfiguration& source, std::pmr::memory_resource* mr) {
    auto clone = eerie_memory::make_shared_pmr<ScreenConfiguration>(mr);
    clone->id = source.id;
    clone->screen_group_id = source.screen_group_id;
    clone->type = source.type;
    clone->z_index = source.z_index;
    clone->is_visible = source.is_visible;
    clone->is_overlay = source.is_overlay;
    clone->grid = source.grid;
    clone->widget_configurations.assign(source.widget_configurations.begin(), source.widget_configurations.end());

    return clone;
}

// The widgets a widget owns, by CHILD_WIDGET_IDS, itself included.
std::vector<uint32_t> OwnedIds(const ScreenConfiguration& screen, uint32_t root_id) {
    std::vector<uint32_t> ids{ root_id };

    for(size_t next = 0; next < ids.size(); ++next) {
        const auto widget = std::ranges::find_if(screen.widget_configurations, [&](const auto& candidate) {
            return candidate->id == ids[next];
        });
        if(widget == screen.widget_configurations.end())
            continue;

        const auto children = (*widget)->properties.find(WidgetPropertyType::CHILD_WIDGET_IDS);
        if(children == (*widget)->properties.end())
            continue;

        const auto* list = std::get_if<std::pmr::vector<int>>(&children->second);
        if(list == nullptr)
            continue;

        for(const auto child : *list) {
            const auto id = static_cast<uint32_t>(child);
            if(std::ranges::find(ids, id) == ids.end())
                ids.push_back(id);
        }
    }

    return ids;
}

const WidgetConfiguration* FindWidget(const ScreenConfiguration& screen, uint32_t id) {
    const auto it = std::ranges::find_if(screen.widget_configurations, [id](const auto& widget) {
        return widget->id == id;
    });

    return it == screen.widget_configurations.end() ? nullptr : it->get();
}

const std::shared_ptr<WidgetConfiguration>* FindWidgetPointer(const ScreenConfiguration& screen, uint32_t id) {
    const auto it = std::ranges::find_if(screen.widget_configurations, [id](const auto& widget) {
        return widget->id == id;
    });

    return it == screen.widget_configurations.end() ? nullptr : &*it;
}

// The root that owns a widget (the widget itself, when it is one), or none if the screen lacks it.
std::optional<uint32_t> RootOf(const ScreenConfiguration& screen, uint32_t widget_id) {
    const auto& widgets = screen.widget_configurations;
    const auto position = std::ranges::find_if(widgets, [widget_id](const auto& widget) {
        return widget->id == widget_id;
    });
    if(position == widgets.end())
        return std::nullopt;

    const auto composition = WidgetComposition::Build(screen);
    std::optional<size_t> index = static_cast<size_t>(position - widgets.begin());
    while(composition.nodes[*index].parent.has_value())
        index = composition.nodes[*index].parent;

    return widgets[*index]->id;
}

// The widgets of a root in the order a screen builds them, as IDs.
std::vector<uint32_t> SubtreeIds(const ScreenConfiguration& screen, const WidgetComposition& composition, size_t root) {
    std::vector<uint32_t> ids;
    std::vector<size_t> pending{ root };
    while(!pending.empty()) {
        const auto index = pending.back();
        pending.pop_back();
        ids.push_back(screen.widget_configurations[index]->id);

        const auto children = composition.GetChildren(index);
        pending.insert(pending.end(), children.rbegin(), children.rend());
    }

    return ids;
}

std::optional<size_t> IndexOf(const ScreenConfiguration& screen, uint32_t id) {
    const auto it = std::ranges::find_if(screen.widget_configurations, [id](const auto& widget) {
        return widget->id == id;
    });

    if(it == screen.widget_configurations.end())
        return std::nullopt;

    return static_cast<size_t>(it - screen.widget_configurations.begin());
}

// The widgets of a screen that stay in place or change, one root at a time.
void PlanWidgets(const ScreenConfiguration& old_screen, const ScreenConfiguration& new_screen, ScreenPatch& patch) {
    const auto old_composition = WidgetComposition::Build(old_screen);
    const auto new_composition = WidgetComposition::Build(new_screen);

    const auto is_root = [](const ScreenConfiguration& screen, const WidgetComposition& composition, uint32_t id) {
        const auto index = IndexOf(screen, id);

        return index.has_value() && !composition.nodes[*index].parent.has_value();
    };

    std::vector<WidgetPatch> changes;

    // Roots that are gone, or that an owner took in.
    for(const auto root : old_composition.roots) {
        const uint32_t id = old_screen.widget_configurations[root]->id;
        if(!is_root(new_screen, new_composition, id))
            patch.widgets.push_back({ .widget_id = id, .action = WidgetAction::Remove });
    }

    for(const auto root : new_composition.roots) {
        const auto& widget = new_screen.widget_configurations[root];

        const auto old_root_index = IndexOf(old_screen, widget->id);
        if(!old_root_index.has_value() || !is_root(old_screen, old_composition, widget->id)) {
            changes.push_back({ .widget_id = widget->id, .action = WidgetAction::Add, .widget = widget });
            continue;
        }

        const auto old_root = *old_root_index;
        const auto old_ids = SubtreeIds(old_screen, old_composition, old_root);
        const auto new_ids = SubtreeIds(new_screen, new_composition, root);

        bool is_subtree_changed = old_ids != new_ids;
        for(size_t i = 1; !is_subtree_changed && i < new_ids.size(); ++i) {
            const auto& before = *FindWidgetPointer(old_screen, new_ids[i]);
            const auto& after = *FindWidgetPointer(new_screen, new_ids[i]);
            is_subtree_changed = before != after && !IsSameWidget(*before, *after);
        }

        const auto& before = old_screen.widget_configurations[old_root];
        if(is_subtree_changed) {
            changes.push_back({ .widget_id = widget->id, .action = WidgetAction::Replace, .widget = widget });
        } else if(before != widget && !IsSameWidget(*before, *widget)) {
            changes.push_back(
                { .widget_id = widget->id,
                    .action = ClassifyWidgetChange(*before, *widget) == WidgetChange::InPlace ? WidgetAction::Update
                                                                                              : WidgetAction::Replace,
                    .widget = widget }
            );
        }
    }

    patch.widgets.insert(patch.widgets.end(), changes.begin(), changes.end());

    // The roots that are in both, in the order each screen draws them: a widget put again after a removal is last
    // among the widgets of its z-index, though it is the same widget.
    const auto root_order = [](const ScreenConfiguration& screen, const WidgetComposition& composition) {
        std::vector<size_t> roots(composition.roots.begin(), composition.roots.end());
        std::ranges::sort(roots, {}, [&screen](size_t i) {
            return std::pair{ screen.widget_configurations[i]->z_index, i };
        });

        std::vector<uint32_t> ids;
        ids.reserve(roots.size());
        for(const auto root : roots)
            ids.push_back(screen.widget_configurations[root]->id);

        return ids;
    };
    const auto old_ids = root_order(old_screen, old_composition);
    const auto new_ids = root_order(new_screen, new_composition);
    const auto in_both = [](const std::vector<uint32_t>& ids, const std::vector<uint32_t>& other) {
        std::vector<uint32_t> kept;
        std::ranges::copy_if(ids, std::back_inserter(kept), [&other](uint32_t id) {
            return std::ranges::find(other, id) != other.end();
        });

        return kept;
    };
    patch.reorders = in_both(old_ids, new_ids) != in_both(new_ids, old_ids);
}

PatchEffect EffectOf(WidgetAction action) {
    return action == WidgetAction::Update ? PatchEffect::InPlace : PatchEffect::WidgetRebuilt;
}

const ScreenPatch* FindScreenPatch(const PatchPlan& plan, uint32_t screen_id) {
    const auto it = std::ranges::find_if(plan.screens, [screen_id](const ScreenPatch& patch) {
        return patch.screen_id == screen_id;
    });

    return it == plan.screens.end() ? nullptr : &*it;
}

const WidgetPatch* FindWidgetPatch(const ScreenPatch& patch, uint32_t root_id) {
    const auto it = std::ranges::find_if(patch.widgets, [root_id](const WidgetPatch& widget) {
        return widget.widget_id == root_id;
    });

    return it == patch.widgets.end() ? nullptr : &*it;
}

PatchEffect WorstEffect(const ScreenPatch& patch) {
    PatchEffect worst = PatchEffect::InPlace;
    for(const auto& widget : patch.widgets)
        worst = std::max(worst, EffectOf(widget.action));

    return worst;
}

} // namespace

WidgetChange ClassifyWidgetChange(const WidgetConfiguration& old_widget, const WidgetConfiguration& new_widget) {
    if(old_widget.type != new_widget.type
        || old_widget.id != new_widget.id
        || !IsSameBindings(old_widget.bindings, new_widget.bindings))
        return WidgetChange::RebuildWidget;

    const auto is_changed = [](const auto& from, const auto& to, WidgetPropertyType type) {
        const auto before = from.find(type);
        const auto after = to.find(type);
        if(before == from.end() || after == to.end())
            return before != from.end() || after != to.end();

        return before->second != after->second;
    };

    // The keys of either record that differ.
    for(const auto& [type, value] : old_widget.properties) {
        if(is_changed(old_widget.properties, new_widget.properties, type) && !AppliesInPlace(new_widget, type))
            return WidgetChange::RebuildWidget;
    }

    for(const auto& [type, value] : new_widget.properties) {
        if(is_changed(old_widget.properties, new_widget.properties, type) && !AppliesInPlace(new_widget, type))
            return WidgetChange::RebuildWidget;
    }

    // The box and the z-index of a root are applied in place; a child never has its own (it fills its owner).
    return WidgetChange::InPlace;
}

WidgetChange ClassifyScreenChange(const ScreenConfiguration& old_screen, const ScreenConfiguration& new_screen) {
    const bool is_same = old_screen.id == new_screen.id
        && old_screen.screen_group_id == new_screen.screen_group_id
        && old_screen.type == new_screen.type
        && old_screen.z_index == new_screen.z_index
        && old_screen.is_visible == new_screen.is_visible
        && old_screen.is_overlay == new_screen.is_overlay
        && IsSameGrid(old_screen.grid, new_screen.grid);

    return is_same ? WidgetChange::InPlace : WidgetChange::RebuildGroup;
}

bool IsSameWidget(const WidgetConfiguration& a, const WidgetConfiguration& b) {
    return a.type == b.type
        && a.id == b.id
        && a.position_grid.x == b.position_grid.x
        && a.position_grid.y == b.position_grid.y
        && a.size_grid.width == b.size_grid.width
        && a.size_grid.height == b.size_grid.height
        && a.z_index == b.z_index
        && IsSameProperties(a.properties, b.properties)
        && IsSameBindings(a.bindings, b.bindings);
}

std::shared_ptr<UiConfiguration>
ApplyPatchOps(const UiConfiguration& base, std::span<const PatchOp> ops, std::pmr::memory_resource* mr) {
    auto patched = eerie_memory::make_shared_pmr<UiConfiguration>(mr);
    patched->active_screen_group_id = base.active_screen_group_id;
    patched->properties = std::pmr::unordered_map<UiPropertyType, ConfigValue>(base.properties, mr);
    patched->screen_configurations.assign(base.screen_configurations.begin(), base.screen_configurations.end());

    // A screen is copied the first time an operation touches it; the widgets it holds are shared.
    std::vector<const ScreenConfiguration*> copies;

    const auto find_screen = [&](uint32_t id) {
        return std::ranges::find_if(patched->screen_configurations, [id](const auto& screen) {
            return screen->id == id;
        });
    };

    const auto editable = [&](uint32_t id) -> ScreenConfiguration& {
        const auto it = find_screen(id);
        if(it == patched->screen_configurations.end())
            throw PatchBaseMismatch("Screen " + std::to_string(id) + " is not in the staged configuration.");

        if(std::ranges::find(copies, it->get()) == copies.end()) {
            *it = CloneScreen(**it, mr);
            copies.push_back(it->get());
        }

        return **it;
    };

    for(const auto& op : ops) {
        switch(op.kind) {
            case PatchOpKind::PutWidget: {
                auto& screen = editable(op.screen_id);
                auto& widgets = screen.widget_configurations;
                const auto it = std::ranges::find_if(widgets, [&op](const auto& widget) {
                    return widget->id == op.widget->id;
                });

                if(it != widgets.end() && (*it)->z_index == op.widget->z_index) {
                    // Stays where it is among the widgets of its z-index.
                    *it = op.widget;
                } else {
                    if(it != widgets.end())
                        widgets.erase(it);

                    screen.AddWidget(op.widget);
                }

                break;
            }

            case PatchOpKind::RemoveWidget: {
                auto& screen = editable(op.screen_id);
                if(FindWidget(screen, op.widget_id) == nullptr)
                    throw PatchBaseMismatch(
                        "Widget " + std::to_string(op.widget_id) + " is not on screen " + std::to_string(op.screen_id)
                        + " of the staged configuration."
                    );

                const auto owned = OwnedIds(screen, op.widget_id);
                std::erase_if(screen.widget_configurations, [&owned](const auto& widget) {
                    return std::ranges::find(owned, widget->id) != owned.end();
                });

                break;
            }

            case PatchOpKind::PutScreen: {
                const auto it = find_screen(op.screen->id);
                if(it != patched->screen_configurations.end())
                    *it = op.screen;
                else
                    patched->screen_configurations.push_back(op.screen);

                // The record is the caller's own copy; it is edited only by operations after this one.
                copies.push_back(op.screen.get());

                break;
            }

            case PatchOpKind::RemoveScreen: {
                const auto it = find_screen(op.screen_id);
                if(it == patched->screen_configurations.end())
                    throw PatchBaseMismatch(
                        "Screen " + std::to_string(op.screen_id) + " is not in the staged configuration."
                    );

                patched->screen_configurations.erase(it);

                break;
            }
        }
    }

    return patched;
}

PatchPlan PlanPatch(const UiConfiguration& base, const UiConfiguration& patched, std::span<const PatchOp> ops) {
    PatchPlan plan;

    const auto find_in = [](const UiConfiguration& configuration, uint32_t id) -> std::shared_ptr<ScreenConfiguration> {
        const auto it = std::ranges::find_if(configuration.screen_configurations, [id](const auto& screen) {
            return screen->id == id;
        });

        return it == configuration.screen_configurations.end() ? nullptr : *it;
    };

    for(const auto& old_screen : base.screen_configurations) {
        if(find_in(patched, old_screen->id) == nullptr)
            plan.screens.push_back(
                { .screen_id = old_screen->id, .action = ScreenAction::Remove, .old_screen = old_screen }
            );
    }

    for(const auto& new_screen : patched.screen_configurations) {
        const auto old_screen = find_in(base, new_screen->id);
        if(old_screen == nullptr) {
            plan.screens.push_back(
                { .screen_id = new_screen->id, .action = ScreenAction::Add, .new_screen = new_screen }
            );
        } else if(old_screen == new_screen) {
            continue;
        } else if(ClassifyScreenChange(*old_screen, *new_screen) == WidgetChange::RebuildGroup) {
            plan.screens.push_back(
                { .screen_id = new_screen->id,
                    .action = ScreenAction::Rebuild,
                    .old_screen = old_screen,
                    .new_screen = new_screen }
            );
        } else {
            ScreenPatch patch{
                .screen_id = new_screen->id,
                .action = ScreenAction::Widgets,
                .old_screen = old_screen,
                .new_screen = new_screen,
            };
            if(old_screen != new_screen)
                PlanWidgets(*old_screen, *new_screen, patch);

            if(!patch.widgets.empty() || patch.reorders)
                plan.screens.push_back(std::move(patch));
        }
    }

    for(const auto& op : ops) {
        const uint32_t screen_id = op.kind == PatchOpKind::PutScreen ? op.screen->id : op.screen_id;
        plan.op_screen_ids.push_back(screen_id);

        const auto* screen_patch = FindScreenPatch(plan, screen_id);
        if(screen_patch == nullptr) {
            plan.effects.push_back(PatchEffect::InPlace);
            continue;
        }

        if(screen_patch->action != ScreenAction::Widgets) {
            plan.effects.push_back(PatchEffect::GroupRebuilt);
            continue;
        }

        switch(op.kind) {
            case PatchOpKind::PutWidget: {
                const auto root = RootOf(*screen_patch->new_screen, op.widget->id);
                const auto* widget_patch = root.has_value() ? FindWidgetPatch(*screen_patch, *root) : nullptr;
                plan.effects.push_back(widget_patch != nullptr ? EffectOf(widget_patch->action) : PatchEffect::InPlace);

                break;
            }

            case PatchOpKind::RemoveWidget: {
                const auto root = RootOf(*screen_patch->old_screen, op.widget_id);
                const auto* widget_patch = root.has_value() ? FindWidgetPatch(*screen_patch, *root) : nullptr;
                plan.effects.push_back(widget_patch != nullptr ? EffectOf(widget_patch->action) : PatchEffect::InPlace);

                break;
            }

            case PatchOpKind::PutScreen:
                plan.effects.push_back(WorstEffect(*screen_patch));
                break;

            case PatchOpKind::RemoveScreen:
                plan.effects.push_back(PatchEffect::GroupRebuilt);
                break;
        }
    }

    return plan;
}

} // namespace eerie_leap::domain::ui_domain::preview
