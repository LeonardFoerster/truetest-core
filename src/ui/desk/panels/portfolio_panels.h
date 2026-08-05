#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/dashboard_snapshot.h"

namespace truetest::ui::desk::panels {

void draw_positions_panel(const dashboard_snapshot& snap);
void draw_lots_panel(const dashboard_snapshot& snap);
void draw_orders_panel(const dashboard_snapshot& snap);
void draw_trade_history_frame();

} // namespace truetest::ui::desk::panels

#endif // HAS_IMGUI_DESK
