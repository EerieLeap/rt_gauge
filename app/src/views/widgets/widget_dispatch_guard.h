#pragma once

#include "subsys/event_bus/dispatch_guard.h"

namespace eerie_leap::views::widgets {

// Widgets use the event bus's guard under their own name. Lock order is always LvglLock (taken by
// the subscriber, see WidgetBase::ResolveBindings) then this lock.
using WidgetDispatchGuard = eerie_leap::subsys::event_bus::DispatchGuard;

} // namespace eerie_leap::views::widgets
