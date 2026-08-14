#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/dashboard_snapshot.h"

namespace truetest::ui::desk::panels {

void draw_equity_panel(const dashboard_snapshot& snap);

} // namespace truetest::ui::desk::panels

#endif // HAS_IMGUI_DESK
