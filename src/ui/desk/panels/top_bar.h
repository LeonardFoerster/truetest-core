#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/dashboard_snapshot.h"
#include "ui/desk/desk_view_model.h"

namespace truetest::ui::desk::panels {

enum class TopBarAction
{
    none,
    pause_toggle,
    flatten,
    kill
};

TopBarAction draw_top_bar(const dashboard_snapshot* snapshot, const CommandCenterViewModel* view,
                          bool paused, bool pause_available, bool pause_state_available,
                          bool flatten_available, bool kill_available);

}  // namespace truetest::ui::desk::panels

#endif  // HAS_IMGUI_DESK
