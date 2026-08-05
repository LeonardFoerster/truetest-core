#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/dashboard_snapshot.h"

#include <cstddef>
#include <vector>

namespace truetest::ui::desk::panels {

void draw_ticker_strip(const dashboard_snapshot& snap);
void draw_equity_panel(const dashboard_snapshot& snap);
void draw_l2_panel(const dashboard_snapshot& snap);
void draw_fills_panel(const dashboard_snapshot& snap,
                      int& filter,
                      std::vector<std::size_t>& visible_rows);
void draw_analysis_frame_panels();

} // namespace truetest::ui::desk::panels

#endif // HAS_IMGUI_DESK
