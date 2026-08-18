#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/dashboard_snapshot.h"
#include "ui/desk/desk_context.h"

namespace truetest::ui {
struct operator_actions;
}

namespace truetest::ui::desk {
class MonitorTelemetry;
}

namespace truetest::ui::desk::panels {

void draw_account_strip(const dashboard_snapshot& snap,
                        const MonitorTelemetry& telemetry);
void draw_safety_strip(const dashboard_snapshot& snap,
                       const operator_actions& actions,
                       const MonitorTelemetry& telemetry);
// Compact MARKET-workspace-only metric band (§7.6): mid/last, spread bps,
// microprice, imbalance, update rate, queue position — nothing that already
// lives in global chrome (target/mode/provider) or the Health panel.
void draw_market_metric_band(const dashboard_snapshot& snap,
                             const MonitorTelemetry& telemetry,
                             const DeskLinkContext& context);
void draw_strategies_panel(const dashboard_snapshot& snap);
void draw_risk_panel(const dashboard_snapshot& snap);
void draw_health_panel(const dashboard_snapshot& snap,
                       const MonitorTelemetry& telemetry);

} // namespace truetest::ui::desk::panels

#endif // HAS_IMGUI_DESK
