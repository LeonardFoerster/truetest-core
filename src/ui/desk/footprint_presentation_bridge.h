#pragma once

#include "analytics/footprint/footprint_aggregator.h"
#include "ui/desk/research_views.h"

// Bridges the cold FootprintAggregator (footprint.md §2.2) into the desk's
// FootprintBarView list (§2.3). Pure/cold - no ImGui, no I/O. Reuses the
// existing ResearchPresentation/FootprintBarView seam rather than inventing
// a parallel "FootprintPresentation" type (imgui.md §0.3: "Never invent
// parallel data paths").
namespace truetest::ui::desk {

struct FootprintPresentationOptions
{
    // footprint.md §1 "base/quote display toggle". false = base-asset
    // quantity per cell; true = quote notional (price * base qty).
    bool quote_units = false;
};

// Exact integer price_ticks/base_qty_atoms are converted to display doubles
// here (and only here) via the aggregator's own tick_size/qty_atom_scale -
// price bucketing itself already happened in integer space upstream (§2.2).
// Bars are returned oldest-first, matching FootprintAggregator::bars().
std::vector<FootprintBarView> to_footprint_bar_views(
    const truetest::footprint::FootprintAggregator& aggregator,
    const FootprintPresentationOptions& options = {});

} // namespace truetest::ui::desk
