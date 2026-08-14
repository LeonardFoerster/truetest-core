#pragma once

#ifdef HAS_IMGUI_DESK

#include "ui/dashboard_snapshot.h"
#include "ui/desk/desk_context.h"
#include "ui/desk/desk_window_names.h"
#include "ui/desk/footprint_camera.h"
#include "ui/desk/footprint_panel_state.h"
#include "ui/desk/research_views.h"

namespace truetest::ui::desk::panels {

void draw_watchlist_panel(const dashboard_snapshot* snap,
                          const ResearchPresentation* research,
                          DeskLinkContext& context,
                          DeskDensity density);
// Returns true when an aggregation-affecting setting changed this frame
// (bar type/interval, tick grouping, imbalance minimum, CVD boundary) - the
// caller (DeskApp, for the demo fixture) is responsible for reconfiguring
// and republishing the underlying FootprintAggregator/ResearchPresentation.
bool draw_orderflow_canvas_panel(const ResearchPresentation* research,
                                 DeskLinkContext& context,
                                 FootprintCamera& camera,
                                 FootprintPanelSettings& settings,
                                 FootprintBoundsCache& bounds_cache,
                                 FootprintViewportCache& viewport_cache);
void draw_dom_panel(DeskPanel panel,
                    const dashboard_snapshot* snap,
                    const ResearchPresentation* research,
                    const DeskLinkContext& context,
                    DeskDensity density);
void draw_selected_context_panel(const dashboard_snapshot* snap,
                                 const ResearchPresentation* research,
                                 const DeskLinkContext& context);
void draw_liquidity_panel(const ResearchPresentation* research,
                          const DeskLinkContext& context);
void draw_liquidations_panel(const ResearchPresentation* research,
                             const DeskLinkContext& context);
void draw_liquidity_tape_panel(const ResearchPresentation* research,
                               const DeskLinkContext& context,
                               DeskDensity density);
void draw_tpo_panel(const ResearchPresentation* research,
                    const DeskLinkContext& context);
void draw_volume_profile_panel(const ResearchPresentation* research,
                               const DeskLinkContext& context);
void draw_session_context_panel(const ResearchPresentation* research,
                                const DeskLinkContext& context);
void draw_funding_panel(const ResearchPresentation* research,
                        const DeskLinkContext& context,
                        DeskDensity density);
void draw_correlation_panel(const ResearchPresentation* research,
                            const DeskLinkContext& context);

} // namespace truetest::ui::desk::panels

#endif
