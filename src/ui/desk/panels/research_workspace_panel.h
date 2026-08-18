#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/desk/desk_capabilities.h"

namespace truetest::ui::desk::panels {

// RESEARCH workspace: Setup | Report | Monte Carlo | Replay. No isolated
// backtest-launcher, resolved-config preview, or AnalyticsReport/MC-report
// seam is wired into the desk today (see docs/internal/imgui-desk-design.md,
// "Research: current vs future wiring") — every tab renders an honest
// NOT WIRED / UNAVAILABLE state rather than reimplementing config precedence
// or inventing report data in UI code.
void draw_research_workspace_panel(const DeskCapabilities& caps);

} // namespace truetest::ui::desk::panels

#endif // HAS_IMGUI_DESK
