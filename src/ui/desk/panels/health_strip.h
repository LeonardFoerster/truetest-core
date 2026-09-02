#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/dashboard_snapshot.h"
#include "ui/desk/desk_view_model.h"

namespace truetest::ui::desk::panels {

void draw_health_strip(const dashboard_snapshot* snapshot, const CommandCenterViewModel* view,
                       bool telemetry_available, bool rate_available);

}  // namespace truetest::ui::desk::panels

#endif  // HAS_IMGUI_DESK
