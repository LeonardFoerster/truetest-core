#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/dashboard_snapshot.h"

namespace truetest::ui::desk {

void draw_debug_panel(const dashboard_snapshot& snap);

} // namespace truetest::ui::desk

#endif // HAS_IMGUI_DESK
